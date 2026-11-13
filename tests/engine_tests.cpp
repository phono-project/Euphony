#include "engine/engine.h"
#include "engine/dictionary.h"
#include "engine/language_model.h"
#include "engine/presentation.h"
#include "engine/shuangpin.h"
#include "engine/worker.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <windows.h>

using namespace euphony;
using json = nlohmann::json;
void check(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
size_t referenceDistance(const std::string& a,const std::string& b) {
    std::vector<std::vector<size_t>> d(a.size()+1,std::vector<size_t>(b.size()+1));
    for(size_t i=0;i<=a.size();++i)d[i][0]=i;
    for(size_t j=0;j<=b.size();++j)d[0][j]=j;
    for(size_t i=1;i<=a.size();++i)for(size_t j=1;j<=b.size();++j)
        d[i][j]=std::min({d[i-1][j]+1,d[i][j-1]+1,d[i-1][j-1]+(a[i-1]!=b[j-1])});
    return d.back().back();
}
struct Client {
    Engine& engine;
    std::string session;
    uint64_t seq = 0;
    json request(std::string op,std::string key={},json extras=json::object()) {
        json input{{"version",1},{"session",session},{"seq",++seq},{"op",op},{"key",key},
            {"sensitive",false},{"context_valid",false},{"surrounding",""}};
        input.update(extras);
        return engine.handle(input);
    }
    json type(std::string text) { json response; for(char c:text) response=request("key",std::string(1,c)); return response; }
};
int main() {
    auto temp = std::filesystem::temp_directory_path() / ("euphony-engine-test-"+std::to_string(GetCurrentProcessId()));
    try {
        std::filesystem::create_directories(temp);
        auto fixture = temp / "dictionary.tsv";
        { std::ofstream out(fixture); out << "你\tni\t-0.1\n尼\tni\t-1\n倪\tni\t-2\n好\thao\t0\n"
            "你好\tni'hao\t0\n中国\tzhong'guo\t0\n吗\tma\t0\n嘛\tma\t-1\n居\tju\t0\n举\tju\t-1\n"; }
        {
            auto path=temp/"segmentation.tsv";
            { std::ofstream out(path); out << "框\tkuang\t0\n哭\tku\t0\n啊\ta\t0\n你\tni\t0\n哥\tge\t0\n安\tan\t0\n西\txi\t0\n先\txian\t0\n"; }
            Dictionary dictionary(path);
            auto shortInput=dictionary.lookup("xian");
            check(std::any_of(shortInput.begin(),shortInput.end(),[](const auto& c){return c.pinyin=="xian";}),"short input keeps whole syllable");
            check(std::any_of(shortInput.begin(),shortInput.end(),[](const auto& c){return c.pinyin=="xi";}),"short input keeps alternate prefix");
            auto longInput=dictionary.lookup("kuangkuang");
            check(!longInput.empty() && longInput[0].pinyin=="kuang'kuang","long input prefers full syllables over ku a n g");
            for(const auto& c:longInput) check(c.pinyin=="kuang" || c.pinyin=="kuang'kuang","long input prunes incompatible syllable paths");
            auto explicitInput=dictionary.lookup("xi'anxi'an");
            check(!explicitInput.empty() && explicitInput[0].pinyin=="xi'an'xi'an","explicit separators override segmentation");
        }
        json config{{"dictionary_path",fixture.string()},{"model_enabled",false},{"input_mode","pinyin"},
            {"learning",true},{"user_directory",temp.string()},{"page_size",3},{"history_characters",32},
            {"engine",{{"schema_version","1.0"},{"tokenizer",{{"max_pinyin_chars",128}}}}},
            {"context",{{"schema_version","1.0"}}}};
        {
            Engine engine(config);
            Client a{engine,"A"}, b{engine,"B"};
            auto answer=a.type("ni");
            check(answer["preedit"]=="ni", "pinyin composition");
            check(answer["candidates"][0]["text"]=="你", "exact syllable outranks longer completion");
            check(answer["cursor"]==2,"UTF-16 cursor");
            b.type("hao");
            check(a.request("poll")["preedit"]=="ni","sessions are isolated");
            const auto chosen=a.request("key","Space");
            check(chosen["commit"]=="你","dictionary space selection");
            check(a.request("poll")["commit"]=="","poll does not replay commit");
            check(engine.handle({{"version",1},{"session","A"},{"seq",a.seq},{"op","key"},{"key","Space"}})["commit"]=="","repeated seq never commits twice");
            check(b.request("key","Enter")["commit"]=="hao","Enter commits raw pinyin");
            a.type("nihao");
            check(a.request("key","CommitRaw")["commit"]=="nihao","mode switch commits raw input");
            a.type("nihao");
            answer=a.request("key","Left");
            check(answer["cursor"]==4,"cursor moves within raw pinyin");
            a.request("key","Home");
            a.request("key","Delete");
            check(a.request("poll")["preedit"]=="ihao","Delete edits at cursor");
            a.request("key","Escape");
            answer=a.type("nim");
            auto index=std::find_if(answer["candidates"].begin(),answer["candidates"].end(),[](const auto& c){ return c["text"]=="你"; })-answer["candidates"].begin();
            check(index<static_cast<decltype(index)>(answer["candidates"].size()),"partial dictionary proposal");
            answer=a.request("key","Select",{{"index",index},{"candidate_generation",answer["candidate_generation"]}});
            check(answer["preedit"]=="你m" && answer["commit"]=="","partial selection keeps composition");
            check(a.request("key","CommitRaw")["commit"]=="nim","raw commit restores partially selected source");
            answer=a.type("nim");
            answer=a.request("key","Select",{{"index",index},{"candidate_generation",answer["candidate_generation"]}});
            check(answer["cursor"]==2,"selected Chinese prefix uses UTF-16 cursor");
            answer=a.request("key","Home");
            answer=a.request("key","Backspace");
            check(answer["preedit"]=="nim","Backspace can undo partial selection");
            a.request("key","Escape");
            auto normalized=a.type("jv");
            check(normalized["candidates"][0]["text"]=="居","immediate dictionary matches core j/q/x/y v normalization");
            a.request("key","Escape");
            answer=a.type("ni");
            const auto stale=answer["candidate_generation"];
            a.request("key","a");
            check(a.request("key","Select",{{"index",0},{"candidate_generation",stale}})["commit"]=="","stale candidate clicks rejected");
            a.request("key","Escape");
            answer=a.type("ni");
            const auto listIdentity=answer["candidate_generation"];
            auto highlighted=a.request("key","Highlight",{{"index",1},{"candidate_generation",listIdentity}});
            check(highlighted["candidate_generation"]==listIdentity && highlighted["generation"]!=answer["generation"],"highlight preserves candidate identity");
            auto finalized=a.request("key","Select",{{"index",1},{"candidate_generation",listIdentity}});
            check(finalized["commit"]=="尼","UIless Highlight then Select can use one candidate identity");
            check(a.request("key",",")["commit"]=="，","Chinese punctuation");
            for (const auto& [key, expected] : std::vector<std::pair<std::string,std::string>>{
                {"\\","、"},{"/","、"},{"|","·"},{"$","¥"},{"^","……"},{"_","——"},{"{","『』"},{"}","』"}})
                check(a.request("key",key)["commit"]==expected,"Chinese punctuation mapping");
            auto pair=a.request("key","(");
            check(pair["commit"]=="（）" && pair["commit_cursor_back"]==1,"paired punctuation positions caret");
            check(a.request("key",".",{{"context_valid",true},{"surrounding","12"}})["eaten"]==false,"decimal keeps ASCII");
            check(a.request("key",",",{{"sensitive",true}})["eaten"]==false,"password punctuation passes through");
            a.type("ni");
            a.request("key","Space",{{"sensitive",true}});
        }
        check(std::filesystem::exists(temp/"learning.json"),"learning file is written");
        std::ifstream learningStream(temp/"learning.json");
        auto learning=json::parse(learningStream);
        learningStream.close();
        check(learning["selections"]["ni\t你"].get<int>()==1,"sensitive or undone partial selection does not learn");
        {
            auto punctuationConfig=config; punctuationConfig["learning"]=false;
            punctuationConfig["user_directory"]=(temp/"punctuation").string();
            Engine punctuation(punctuationConfig); Client client{punctuation,"punctuation"};
            client.type("ni");
            check(client.request("key","\\")["commit"]=="你、","Backslash confirms composition and inserts enumeration comma");
        }
        {
            Dictionary persisted(fixture);
            persisted.loadLearning(temp/"learning.json");
            persisted.learn("ni'hao'ma","你好吗");
            persisted.saveLearning(temp/"learning.json");
            auto disabledConfig=config;disabledConfig["learning"]=false;
            {
                Engine disabled(disabledConfig);Client client{disabled,"learning-off"};
                auto phrase=client.type("nihaoma");
                check(phrase["candidates"][0]["text"]=="你好吗","learning off preserves existing user phrases");
                check(client.request("key","Space")["commit"]=="你好吗","existing learned phrase remains selectable");
            }
            std::ifstream preservedStream(temp/"learning.json");
            auto preserved=json::parse(preservedStream);
            check(preserved["selections"]["ni'hao'ma\t你好吗"]==1,"learning off does not record a new selection");
        }
        for (bool full : {false,true}) for (bool digits : {false,true}) for (bool symbols : {false,true}) for (bool chinese : {false,true}) {
            auto settings=config; settings["full_width"]=full; settings["full_width_digits"]=digits;
            settings["full_width_symbols"]=symbols; settings["chinese_punctuation"]=chinese; settings["learning"]=false;
            Engine engine(settings); Client client{engine,"width-options"};
            auto number=client.request("key","2");
            check(number["eaten"]==(full && digits),"digit conversion follows full-width preference");
            if(full && digits) check(number["commit"]=="２","enabled digits use full-width form");
            if (chinese) check(client.request("key","$")["commit"]==(full && symbols ? "￥" : "¥"),"Currency width follows functional-symbol preference");
            for(const auto& [key,wide] : std::vector<std::pair<std::string,std::string>>{
                {"~","～"},{"@","＠"},{"#","＃"},{"%","％"},{"&","＆"},{"*","＊"},{"-","－"},{"=","＝"},{"+","＋"},{"|","｜"},{"/","／"}}) {
                auto result=client.request("key",key);
                if(full && symbols) check(result["commit"]==wide,"enabled functional symbols use full-width form");
                else if(chinese && (key=="|" || key=="/")) check(result["commit"]==(key=="|"?"·":"、"),"disabled symbols preserve half-width Chinese mapping");
                else check(result["eaten"]==false,"disabled symbols pass through as ASCII");
            }
        }
        {
            auto widthConfig=config; widthConfig["full_width"]=true; widthConfig["full_width_digits"]=true; widthConfig["chinese_punctuation"]=false;
            Engine fullWidth(widthConfig); Client client{fullWidth,"width"};
            check(client.request("key","1")["commit"]=="１","full-width digits");
            check(client.request("key","A",{{"shift",true}})["commit"]=="Ａ","full-width uppercase");
            check(client.request("key","Space")["commit"]=="　","idle full-width space");
            check(client.request("key",",")["commit"]=="，","full-width ASCII punctuation");
            check(client.request("key","n")["preedit"]=="n","full-width mode still accepts pinyin");
        }
        Dictionary real(std::filesystem::path(EUPHONY_TEST_DATA_DIR)/"libime.tsv");
        check(real.size()>250000,"substantive libime dictionary is shipped");
        check(!real.lookup("nihao").empty(),"real dictionary nihao");
        {
            auto arpa=temp/"ranking.arpa";
            {std::ofstream out(arpa);out << "\\data\\\nngram 1=12\nngram 2=7\n\n\\1-grams:\n"
                "-8\t<unk>\t0\n-1\t<s>\t0\n-1\t</s>\t0\n"
                "-0.1\t你\t0\n-3\t尼\t0\n-4\t倪\t0\n-0.2\t好\t0\n"
                "-0.3\t吗\t0\n-2\t嘛\t0\n-2.3\t你好\t0\n-6\t倪好\t0\n-2\t居\t0\n"
                "\n\\2-grams:\n-0.1\t<s> 你\n-3\t<s> 尼\n-0.01\t你 好\n-3\t尼 好\n"
                "-0.01\t好 尼\n-4\t好 你\n-0.01\t好 吗\n\n\\end\\\n";}
            auto model=std::make_shared<LanguageModel>(arpa);
            check(model->order()==2 && model->known("你"),"KenLM ARPA vocabulary loads");
            check(model->unigram("你")>model->unigram("尼"),"KenLM null state provides real unigram order");
            auto contextual=model->contextState("好");LanguageModel::State next;
            check(model->score(contextual,"尼",next)>model->score(contextual,"你",next),"KenLM uses preceding word bigrams");
            auto segmentedFile=temp/"segmentation.tsv";
            {std::ofstream out(segmentedFile);out<<"你\tni\t0\n尼\tni\t0\n好\thao\t0\n倪好\tni'hao\t0\n";}
            Dictionary segmented(segmentedFile);segmented.setLanguageModel(model);
            check(segmented.lookup("nihao")[0].text=="你好","KenLM can prefer a split path over an existing low-probability whole word");
            check(segmented.lookup("ni",64,"好")[0].text=="尼","dictionary ranking uses real surrounding context");
            auto truncateFile=temp/"truncation.tsv";
            {std::ofstream out(truncateFile);for(int i=0;i<70;++i)out<<"罕"<<i<<"\tni\t0\n";out<<"你\tni\t-0.1\n";}
            Dictionary truncated(truncateFile);truncated.setLanguageModel(model);
            check(truncated.lookup("ni")[0].text=="你","LM ranks before the candidate limit is applied");
            auto routeConfig=config;
            routeConfig["language_model_path"]=arpa.string();routeConfig["model_enabled"]=true;
            routeConfig["model_directory"]=temp.string();
            routeConfig["user_directory"]="";
            Engine routed(routeConfig);Client routing{routed,"routing"};
            auto status=routed.handle({{"op","status"}});
            check(status["language_model_ready"]==true && status["language_model_error"]=="","LM readiness is exposed to settings");
            auto single=routing.type("ni");
            check(single["candidate_source"]=="unigram" && !single["pending"].get<bool>() && single["candidates"][0]["text"]=="你", "default single complete syllable avoids deep inference");
            for(const auto& candidate:single["candidates"])
                check(!candidate["model"].get<bool>() && utf8Tail(candidate["text"].get<std::string>(),1)==candidate["text"].get<std::string>(),"unigram route contains single-character dictionary words only");
            auto withContext=routing.request("poll",{},{{"context_valid",true},{"surrounding","好"}});
            check(withContext["candidate_source"]=="dictionary" && withContext["pending"]==true,"new surrounding context restores deep inference");
            routing.request("reset");
            auto incomplete=routing.type("n");
            check(incomplete["candidate_source"]!="unigram" && incomplete["pending"]==true,"initial abbreviation is not a complete syllable");
            routing.request("reset");
            auto multiple=routing.type("nihao");
            check(multiple["candidate_source"]!="unigram" && multiple["pending"]==true,"multiple syllables retain deep inference");
            routeConfig["prefer_unigram_single_syllable"]=false;
            Engine disabled(routeConfig);Client toggle{disabled,"toggle"};
            auto bypassed=toggle.type("ni");
            check(bypassed["candidate_source"]!="unigram" && bypassed["pending"]==true,"single-syllable switch restores deep inference");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto frozen=toggle.request("key","Highlight",{{"index",0},{"candidate_generation",bypassed["candidate_generation"]}});
            check(frozen["candidate_generation"]==bypassed["candidate_generation"] && !frozen["pending"].get<bool>() && frozen["error"]=="",
                "a displayed-list interaction freezes queued completion before it can replace the list");
            auto stillFrozen=toggle.request("poll");
            check(stillFrozen["candidate_generation"]==frozen["candidate_generation"] && stillFrozen["error"]=="","cancelled completion never changes a frozen displayed list");
            routeConfig["prefer_unigram_single_syllable"]=true;routeConfig["input_mode"]="shuangpin";
            Engine shuangpin(routeConfig);Client pairs{shuangpin,"pair-route"};
            check(pairs.type("ni")["candidate_source"]=="unigram","one complete shuangpin pair uses unigram ranking");
            pairs.request("reset");
            check(pairs.type("nihc")["pending"]==true,"two shuangpin syllables retain deep inference");
            routeConfig["language_model_path"]=(temp/"missing.lm").string();
            Engine missing(routeConfig);Client fallback{missing,"missing-lm"};
            check(missing.handle({{"op","status"}})["language_model_ready"]==false,"missing LM is reported");
            check(fallback.type("ni")["pending"]==true,"missing LM preserves the normal deep-model path");
        }
        {
            const auto modelPath=std::filesystem::path(EUPHONY_TEST_DATA_DIR)/"zh_CN.lm";
            auto official=std::make_shared<LanguageModel>(modelPath);
            check(official->order()>=2,"official libime model contains n-gram statistics");
            check(official->unigram("你")>official->unigram("㘈"),"official LM ranks common ni above a rare character");
            real.setLanguageModel(official);
            auto common=real.lookup("ni",64,{},true);
            check(!common.empty() && common[0].text=="你","actual libime data ranks ni by frequency");
            check(real.lookup("nihao")[0].text=="你好","actual libime n-grams rank phrase segmentation");
            auto unicodeFile=temp/L"语言模型.lm";
            // CI can place the checkout and system temporary directory on different volumes.
            check(std::filesystem::copy_file(modelPath,unicodeFile),"Unicode language-model filename fixture is created");
            {LanguageModel unicode(unicodeFile);check(unicode.known("你"),"KenLM loads a real Chinese filename through its native path adapter");}
        }
        {
            auto memoryConfig=config;
            const auto directory=temp/"memory";
            memoryConfig["user_directory"]=directory.string();
            memoryConfig["user_candidate_color"]="#CC2233";
            {
                Engine engine(memoryConfig); Client client{engine,"memory"};
                auto answer=client.type("nihaoma");
                for (const auto* text : {"倪","好","嘛"}) {
                    const auto& list=answer["candidates"];
                    auto found=std::find_if(list.begin(),list.end(),[&](const auto& c){return c["text"]==text;});
                    check(found!=list.end(),"manual phrase component exists");
                    answer=client.request("key","Select",{{"index",found-list.begin()},{"candidate_generation",answer["candidate_generation"]}});
                }
                check(answer["commit"]=="倪好嘛","whole manually composed phrase commits");
                answer=client.type("nihaoma");
                check(answer["candidates"][0]["text"]=="倪好嘛" && answer["candidates"][0]["user"]==true,"remembered phrase leads immediately");
                check(answer["user_candidate_color"]=="#CC2233","configured memory color reaches frontend");
                client.request("key","Space");
            }
            Dictionary restored(fixture);restored.loadLearning(directory/"learning.json");
            check(restored.frequency("ni'hao'ma","倪好嘛")==2,"whole phrase count persists");
            memoryConfig["model_enabled"]=true;memoryConfig["model_directory"]=temp.string();
            {
                Engine engine(memoryConfig,[](const Request&) {Result result;result.ready=true;result.candidates={"你好嘛"};return result;});
                Client client{engine,"memory-model"};auto answer=client.type("nihaoma");
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
                while(answer["pending"]==true && std::chrono::steady_clock::now()<deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));answer=client.request("poll");
                }
                check(answer["pending"]==false,"memory model fixture completes");
                check(answer["candidates"][0]["user"]==true && answer["candidates"][0]["model"]==false,"memory leads distinct model result");
                answer=client.request("key","Delete",{{"candidate_generation",answer["candidate_generation"]}});
                check(answer["preedit"]=="nihaoma" && answer["commit"]=="","forget preserves input");
                for(const auto& c:answer["candidates"])check(c["user"]==false,"forget immediately removes memory priority");
            }
            Dictionary forgotten(fixture);forgotten.loadLearning(directory/"learning.json");
            check(forgotten.frequency("ni'hao'ma","倪好嘛")==0,"forgotten phrase stays forgotten after restart");
            auto pairConfig=memoryConfig;pairConfig["model_enabled"]=false;pairConfig["input_mode"]="shuangpin";pairConfig["shuangpin_scheme"]="xiaohe";
            restored.saveLearning(directory/"learning.json");
            Dictionary afterStaleSave(fixture);afterStaleSave.loadLearning(directory/"learning.json");
            check(afterStaleSave.frequency("ni'hao'ma","倪好嘛")==0,"stale engine save does not resurrect deletion");
            forgotten.learn("ni'hao'ma","倪好嘛");forgotten.saveLearning(directory/"learning.json");
            Engine pairs(pairConfig);Client pair{pairs,"memory-pairs"};auto answer=pair.type("nihcma");
            check(answer["candidates"][0]["text"]=="倪好嘛" && answer["candidates"][0]["user"]==true,"shuangpin reuses canonical phrase");
            pair.request("key","Delete");
            Dictionary deletedAgain(fixture);deletedAgain.loadLearning(directory/"learning.json");
            check(deletedAgain.frequency("ni'hao'ma","倪好嘛")==0,"shuangpin deletion persists");
        }
        for(const auto& interaction:std::vector<std::string>{"Select","Space","Highlight"}) {
            auto raceConfig=config;raceConfig["model_enabled"]=true;
            raceConfig["model_directory"]=temp.string();
            raceConfig["user_directory"]="";
            std::atomic_bool release=false;
            Engine engine(raceConfig,[&](const Request& request) {
                Result result;result.ready=true;
                if(request.pinyin!="ni")return result;
                while(!release.load() && !request.cancelled->load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
                result.candidates={"倪"};return result;
            });
            Client client{engine,"shown-list"};
            auto shown=client.type("ni");
            const auto displayedToken=shown["candidate_generation"];
            check(shown["candidates"][0]["text"]=="你","race fixture begins with a displayed dictionary list");
            release.store(true);
            json updated;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            do {
                updated=client.request("poll");
                if(updated["candidate_source"]=="phono")break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while(std::chrono::steady_clock::now()<deadline);
            check(updated["candidate_source"]=="phono" && updated["candidates"][0]["text"]=="倪" &&
                updated["candidate_generation"]!=displayedToken,"a model poll changes the broker list before the displayed list changes");
            const auto newerToken=updated["candidate_generation"];
            auto selected=client.request("key",interaction,{{"index",1},{"candidate_generation",displayedToken}});
            if(interaction=="Highlight") {
                check(selected["candidate_generation"]==displayedToken && selected["selection"]==1,"previous displayed list remains stable after UIless highlight");
                selected=client.request("key","Select",{{"index",1},{"candidate_generation",displayedToken}});
            }
            check(selected["commit"]==(interaction=="Space"?"你":"尼"),"an old displayed token selects its original candidate after a same-input model poll");
            auto next=client.type("ni");
            check(next["candidate_generation"].get<uint64_t>()>newerToken.get<uint64_t>(),"restoring a displayed token never reuses future candidate token IDs");
            const auto oldInputToken=next["candidate_generation"];
            client.request("key","a");
            check(client.request("key","Select",{{"index",0},{"candidate_generation",oldInputToken}})["commit"]=="","old tokens are rejected after the input changes");
        }
        {
            Dictionary combined(fixture);
            auto proposals=combined.lookup("nihaoma");
            check(!proposals.empty() && proposals[0].text=="你好吗","dictionary combination keeps best beam first");
            std::unordered_set<std::string> texts;
            for(const auto& proposal:proposals)
                check(texts.insert(proposal.text+"\t"+std::to_string(proposal.consumed)).second,"dictionary combinations are deduplicated");
            auto completionFile=temp/"completion.tsv";
            {std::ofstream out(completionFile);out<<"你好\tni'hao\t0\n你好吗\tni'hao'ma\t10\n";}
            Dictionary completion(completionFile);
            auto incomplete=completion.lookup("niha");
            check(!incomplete.empty() && incomplete[0].text=="你好","completion extends only the last entered syllable");
            for(const auto& proposal:incomplete)check(proposal.text!="你好吗","completion does not invent another syllable");
        }
        for (const auto& mode : {"pinyin", "shuangpin"}) for (bool enabled : {false,true}) for (bool first : {false,true}) {
            const auto directory=temp/(std::string("user-options-")+mode+(enabled?"-on":"-off")+(first?"-first":"-after"));
            Dictionary personal(fixture);personal.learn("ni'hao'ma","倪好嘛");personal.saveLearning(directory/"learning.json");
            auto settings=config;settings["user_directory"]=directory.string();settings["input_mode"]=mode;
            settings["user_words_enabled"]=enabled;settings["user_words_first"]=first;settings["learning"]=false;
            settings["model_enabled"]=true;settings["model_directory"]=temp.string();
            {
                Engine engine(settings,[](const Request&) {Result result;result.ready=true;result.candidates={"模型整句"};return result;});
                Client client{engine,"user-options"};auto answer=client.type(std::string(mode)=="pinyin" ? "nihaoma" : "nihcma");
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
                while(answer["pending"]==true && std::chrono::steady_clock::now()<deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));answer=client.request("poll");
                }
                check(answer["pending"]==false,"user option fixture completes");
                check(answer["candidates"][0]["text"]==((enabled && first)?"倪好嘛":"模型整句"),"user word enable and placement settings control model ordering");
                if (enabled) {
                    const auto& list=answer["candidates"];
                    auto found=std::find_if(list.begin(),list.end(),[](const auto& c){return c["user"]==true;});
                    check(found!=list.end(),"user word remains available after model results");
                    answer=client.request("key","Highlight",{{"index",found-list.begin()},{"candidate_generation",answer["candidate_generation"]}});
                    answer=client.request("key","Delete",{{"candidate_generation",answer["candidate_generation"]}});
                    check(answer["commit"]=="" && answer["pending"]==true,"deleting a user word regenerates candidates without committing");
                    for (const auto& c:answer["candidates"])check(c["user"]==false,"deleted word no longer carries user identity");
                } else for (const auto& c:answer["candidates"])check(c["user"]==false,"disabled memories do not mark native candidates as user words");
            }
            Dictionary restored(fixture);restored.loadLearning(directory/"learning.json");
            check(restored.frequency("ni'hao'ma","倪好嘛")==uint32_t(enabled?0:1),"disabling user words retains memory and explicit deletion persists with learning off");
            if (!enabled) check(restored.lookup("nihaoma")[0].text=="倪好嘛","retained memories work when enabled again");
        }
        for (const auto& mode : {"pinyin", "shuangpin"}) for (bool exactOnly : {false,true}) {
            const auto directory=temp/(std::string("exact-memory-")+mode+(exactOnly?"-on":"-off"));
            Dictionary personal(fixture);personal.learn("ni'hao","倪好");personal.saveLearning(directory/"learning.json");
            auto settings=config;settings["user_directory"]=directory.string();settings["input_mode"]=mode;
            settings["user_words_exact_only"]=exactOnly;
            settings["model_enabled"]=true;settings["model_directory"]=temp.string();
            Engine engine(settings,[](const Request&) {Result result;result.ready=true;result.candidates={"模型整句"};return result;});
            Client client{engine,"exact-memory"};
            for (int kind : {0,1,2}) {
                const bool pairs=std::string(mode)=="shuangpin";
                const auto input=kind==0 ? (pairs?"nihc":"nihao") : kind==1 ? (pairs?"nihcma":"nihaoma") : (pairs?"nih":"niha");
                auto answer=client.type(input);
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
                while(answer["pending"]==true && std::chrono::steady_clock::now()<deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));answer=client.request("poll");
                }
                check(answer["pending"]==false,"exact-match fixture completes");
                check(answer["candidates"][0]["text"]==((!exactOnly || kind==0)?"倪好":"模型整句"),"exact-match setting orders whole, prefix and unfinished user words");
                client.request("reset");
            }
        }
        for (const auto& mode : {"pinyin", "shuangpin"}) {
            const auto directory=temp/(std::string("single-memory-")+mode);
            Dictionary personal(fixture);personal.learn("ni","倪");personal.saveLearning(directory/"learning.json");
            auto settings=config;settings["user_directory"]=directory.string();settings["input_mode"]=mode;
            settings["user_words_exact_only"]=false;
            settings["model_enabled"]=true;settings["model_directory"]=temp.string();
            Engine engine(settings,[](const Request&) {Result result;result.ready=true;result.candidates={"模型整句"};return result;});
            Client client{engine,"single-memory"};auto answer=client.type(std::string(mode)=="pinyin" ? "nihaoma" : "nihcma");
            check(answer["candidates"][0]["text"]!="倪","single memory does not lead before inference");
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(answer["pending"]==true && std::chrono::steady_clock::now()<deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));answer=client.request("poll");
            }
            check(answer["candidates"][0]["text"]=="模型整句","single memory stays behind model for longer input");
        }
        {
            Dictionary personal(fixture);
            personal.learn("ni","倪");
            check(personal.lookup("ni")[0].text=="倪","single-character memory leads for single-syllable input");
            check(personal.lookup("nihaoma")[0].text!="倪","single-character memory does not lead for longer input");
            personal.learn("ni'hao'ma","你好吗");
            check(personal.lookup("nihaoma")[0].text=="你好吗","new explicit dictionary phrase is reusable");
            auto personalFile=temp/"personal.json";
            personal.saveLearning(personalFile);
            Dictionary restored(fixture);restored.loadLearning(personalFile);
            check(restored.lookup("nihaoma")[0].text=="你好吗","learned phrases survive restart");
            Dictionary older(fixture),newer(fixture);
            older.loadLearning(personalFile);newer.loadLearning(personalFile);
            older.learn("ni","你");newer.learn("ni","你");
            older.saveLearning(personalFile);newer.saveLearning(personalFile);older.saveLearning(personalFile);
            std::ifstream mergedStream(personalFile);
            auto merged=json::parse(mergedStream);
            check(merged["selections"]["ni\t你"]==2,"retained engines preserve both learning increments");
        }
        for (const auto& example : std::vector<std::pair<std::string,std::string>>{
            {"ziranma","nihkma"},{"ms","nihkma"},{"ziguang","nihqma"},{"abc","nihkma"},
            {"zhongwenzhixing","nihdma"},{"pinyinjiajia","nihdma"},{"xiaohe","nihcma"}}) {
            ShuangpinProfile profile(example.first,real.syllables());
            auto converted=profile.convert(example.second);
            check(converted.tokens.size()==3 && converted.tokens[0].alternatives[0]=="ni" &&
                converted.tokens[1].alternatives[0]=="hao" && converted.tokens[2].alternatives[0]=="ma","seven shuangpin profiles");
            check(profile.convert("u'i'v").tokens.empty(),"lone u/i/v remain invalid");
            check(profile.convert("lo").tokens[0].alternatives==std::vector<std::string>{"lo","luo"},"shuangpin ambiguity preserved");
        }
        {
            auto settings=config; settings["model_enabled"]=true; settings["model_directory"]=temp.string(); settings["user_directory"]="";
            std::atomic_bool leaked=false;
            Engine engine(settings,[&](const Request& request) { if (!request.history.empty()) leaked=true; Result result;result.ready=true;return result; });
            Client client{engine,"console"};
            client.request("key","n",{{"context_disabled",true},{"context_valid",true},{"surrounding","旧上下文"}});
            client.request("key","CommitRaw",{{"context_disabled",true}});
            client.request("key","h",{{"context_disabled",true}});
            for(int i=0;i<100;++i) { auto answer=client.request("poll",{},{{"context_disabled",true}}); if(!answer["pending"].get<bool>())break; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
            check(!leaked.load(),"console ignores surrounding text and committed history");
        }
        {
            auto englishFile=temp/"english.tsv";
            {std::ofstream out(englishFile);out<<"nihaoma\t30\nnihao\t10\nnihc\t3\nhello\t5\nhelloworld\t20\n";}
            EnglishDictionary words(englishFile);
            check(words.lookup("hello")[0]=="hello","exact English word beats more frequent completion");
            check(words.lookup("hell")[0]=="hello","English excludes distant prefix completions");
            check(words.lookup("hello'").empty() && words.lookup("zzzz").empty(),"English ignores separators and absent prefixes");
            EnglishDictionary bundled(std::filesystem::path(EUPHONY_TEST_DATA_DIR)/"english.tsv");
            check(bundled.size()>=370000 && bundled.lookup("euphony")[0]=="euphony","bundled English dictionary includes broad vocabulary");
            const auto started=std::chrono::steady_clock::now();
            for(int i=0;i<100;++i)bundled.lookup(i%2?"hello":"configuration");
            std::cout<<"100 bundled English lookups: "<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count()<<" ms\n";
            // Compare the pruned search to an independent full scan across many
            // length groups and lexicographic bucket boundaries.
            std::vector<std::string> vocabulary,level={""};
            for(int length=1;length<=6;++length) {
                std::vector<std::string> next;
                for(const auto& prefix:level)for(char c:std::string("abc"))next.push_back(prefix+c);
                vocabulary.insert(vocabulary.end(),next.begin(),next.end());level=std::move(next);
            }
            auto bucketFile=temp/"english-buckets.tsv";
            {std::ofstream out(bucketFile);for(size_t i=vocabulary.size();i>0;--i)out<<vocabulary[i-1]<<'\t'<<(i%53)<<'\n';}
            EnglishDictionary bucketed(bucketFile);
            std::vector<std::string> queries={"z","abcdefg","abdc","aaaaaaa","c"};
            for(size_t i=0;i<vocabulary.size();i+=37)queries.push_back(vocabulary[i]);
            for(const auto& query:queries)for(size_t threshold:{0,1,2,3,10}) {
                std::vector<size_t> expected;
                for(size_t i=0;i<vocabulary.size();++i)if(referenceDistance(query,vocabulary[i])<=threshold)expected.push_back(i);
                std::sort(expected.begin(),expected.end(),[&](size_t a,size_t b) {
                    if((vocabulary[a]==query)!=(vocabulary[b]==query))return vocabulary[a]==query;
                    if((a+1)%53!=(b+1)%53)return (a+1)%53>(b+1)%53;
                    return vocabulary[a]<vocabulary[b];
                });
                for(size_t limit:{1,5,2000}) {
                    std::vector<std::string> reference;
                    for(size_t i=0;i<std::min(limit,expected.size());++i)reference.push_back(vocabulary[expected[i]]);
                    check(bucketed.lookup(query,limit,threshold)==reference,"triangle pruning and banded distance match full Levenshtein scan");
                }
            }
            check(words.lookup("helloworld",5,0)==std::vector<std::string>{"helloworld"},"zero distance permits exact English only");
            check(words.lookup("helloworld",0,1).empty(),"zero result limit stays empty");
            check(words.lookup("hallo")==std::vector<std::string>{"hello"},"one substitution is accepted at the inclusive distance boundary");
            check(words.lookup("helllo")==std::vector<std::string>{"hello"},"one deletion is accepted");
            check(words.lookup("hell")==std::vector<std::string>{"hello"},"one insertion is accepted");
            check(words.lookup("hlelo").empty(),"transposition counts as two Levenshtein edits");
            for(int threshold:{0,1}) {
                auto settings=config;settings["english_dictionary_path"]=englishFile.string();settings["english_max_distance"]=threshold;settings["user_directory"]="";
                Engine filtered(settings);Client client{filtered,"distance-setting"};auto answer=client.type("hallo");
                const bool matched=std::any_of(answer["candidates"].begin(),answer["candidates"].end(),[](const auto& c){return c["english"]==true;});
                check(matched==(threshold==1),"engine applies the configured English edit distance");
            }
            auto directory=temp/"english-learning";std::filesystem::create_directories(directory);
            Dictionary personal(fixture);personal.learn("ni'hao","倪好");personal.saveLearning(directory/"learning.json");
            auto wait=[](Client& client,json answer) {
                for(int i=0;i<1000 && answer["pending"].get<bool>();++i) {std::this_thread::sleep_for(std::chrono::milliseconds(1));answer=client.request("poll");}
                check(!answer["pending"].get<bool>(),"English ranking inference completed");return answer;
            };
            for(bool userFirst:{false,true}) for(bool englishFirst:{false,true}) {
                auto settings=config;settings["model_enabled"]=true;settings["model_directory"]=temp.string();
                settings["user_directory"]=directory.string();settings["learning"]=false;
                settings["english_dictionary_path"]=englishFile.string();settings["english_max_distance"]=2;settings["user_words_first"]=userFirst;settings["english_words_first"]=englishFirst;
                Engine engine(settings,[](const Request&){Result result;result.ready=true;result.candidates={"模型词","模型二"};return result;});
                Client client{engine,"placement"};auto answer=wait(client,client.type("nihao"));
                std::vector<std::string> expected;
                if(userFirst)expected.push_back("倪好");
                if(englishFirst){expected.push_back("nihao");expected.push_back("nihaoma");expected.push_back("nihc");}
                expected.push_back("模型词");expected.push_back("模型二");
                if(!userFirst)expected.push_back("倪好");
                if(!englishFirst){expected.push_back("nihao");expected.push_back("nihaoma");expected.push_back("nihc");}
                for(size_t i=0;i<expected.size();++i)check(answer["candidates"][i]["text"]==expected[i],"before/after lists preserve user then English provider order");
                auto index=std::find_if(answer["candidates"].begin(),answer["candidates"].end(),[](const auto& c){return c["text"]=="nihao";})-answer["candidates"].begin();
                check(answer["candidates"][index]["english"]==true,"English provenance is exposed for highlighting");
                check(client.request("key","Select",{{"index",index},{"candidate_generation",answer["candidate_generation"]}})["commit"]=="nihao","English commits the word directly");
            }
            auto settings=config;settings["model_enabled"]=true;settings["model_directory"]=temp.string();settings["user_directory"]=directory.string();settings["learning"]=false;
            settings["english_dictionary_path"]=englishFile.string();settings["english_min_letters"]=5;
            Engine engine(settings,[](const Request&){Result result;result.ready=true;result.candidates={"倪好","nihao"};return result;});
            Client client{engine,"provenance"};auto answer=wait(client,client.type("niha"));
            for(const auto& candidate:answer["candidates"])check(candidate["english"]==false,"English minimum length is configurable");
            answer=wait(client,client.type("o"));
            check(answer["candidates"][0]["text"]=="倪好" && answer["candidates"][0]["model"]==true && answer["candidates"][0]["user"]==false,"model wins duplicate user provenance");
            check(answer["candidates"][1]["text"]=="nihao" && answer["candidates"][1]["model"]==true && answer["candidates"][1]["english"]==false,"model wins duplicate English provenance");
            settings["candidate_source_priority"]={"english","user","model"};
            Engine custom(settings,[](const Request&){Result result;result.ready=true;result.candidates={"倪好","nihao"};return result;});
            Client customClient{custom,"custom-priority"};auto customAnswer=wait(customClient,customClient.type("nihao"));
            check(customAnswer["candidates"][0]["user"]==true && customAnswer["candidates"][1]["english"]==true,"hidden source priority controls duplicate provenance");
            settings["model_enabled"]=false;settings["input_mode"]="shuangpin";settings["english_min_letters"]=4;
            Engine pairs(settings);Client pair{pairs,"english-pairs"};answer=pair.type("nihc");
            check(answer["candidates"][0]["text"]=="倪好" && answer["candidates"][1]["text"]=="nihc","shuangpin English matches original letters");
        }
        auto presentation=presentCandidate("你好",{{2,3,1,"!"}},InvalidInputPolicy::Delete);
        check(presentation.committed=="你好" && presentation.pieces[1].deleted,"invalid text uses struck-out presentation");
        check(utf16Length("你😀a")==4,"UTF-16 supplementary character count");
        check(utf8Tail("你😀好",2)=="😀好","history truncates UTF-8 characters");
        {
            std::mutex mutex; std::condition_variable ready;
            bool entered=false, completed=false;
            Worker worker([&](const Request& request) {
                {std::lock_guard lock(mutex); entered=true;} ready.notify_all();
                while(!request.cancelled->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return Result{};
            });
            Request request; request.contextId=1;
            auto cancellation=request.cancelled;
            worker.submit(request,[&](Result){completed=true;});
            {std::unique_lock lock(mutex); ready.wait_for(lock,std::chrono::seconds(2),[&]{return entered;});}
            cancellation->store(true); worker.stop();
            check(entered && !completed,"cancelled model result never completes");
        }
        std::cout<<"Engine tests passed; "<<real.size()<<" libime entries.\n";
        std::filesystem::remove_all(temp);
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        std::filesystem::remove_all(temp);
        return 1;
    }
}

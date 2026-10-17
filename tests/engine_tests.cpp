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
        {
            auto widthConfig=config; widthConfig["full_width"]=true; widthConfig["chinese_punctuation"]=false;
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
            routeConfig["core_library"]="C:/euphony-test-missing-core.dll";routeConfig["model_directory"]=temp.string();
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
            memoryConfig["model_enabled"]=true;memoryConfig["model_directory"]=temp.string();memoryConfig["core_library"]="test-inference";
            {
                Engine engine(memoryConfig,[](const Request&) {Result result;result.ready=true;result.candidates={"你好嘛","倪好嘛"};return result;});
                Client client{engine,"memory-model"};auto answer=client.type("nihaoma");
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
                while(answer["pending"]==true && std::chrono::steady_clock::now()<deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));answer=client.request("poll");
                }
                check(answer["pending"]==false,"memory model fixture completes");
                check(answer["candidates"][0]["user"]==true && answer["candidates"][0]["model"]==false,"memory leads and wins duplicate model identity");
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
            raceConfig["core_library"]="test-inference";raceConfig["model_directory"]=temp.string();
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
        {
            Dictionary personal(fixture);
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

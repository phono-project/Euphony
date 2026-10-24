#include "engine.h"
#include "dictionary.h"
#include "language_model.h"
#include "presentation.h"
#include "shuangpin.h"
#include "worker.h"
#include "common/win_util.h"
#include <algorithm>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_set>

namespace euphony {
using json = nlohmann::json;
namespace {
struct Candidate {
    std::string text, commit, pinyin;
    bool model = false;
    size_t consumed = 0;
    std::vector<CandidatePiece> pieces;
    bool user = false;
    bool preferred = false;
    bool operator==(const Candidate&) const = default;
};
struct Selection { std::string source, text, pinyin; };
struct CandidateSnapshot {
    uint64_t generation = 0;
    size_t selection = 0;
    std::string source;
    std::vector<Candidate> candidates;
};
struct State {
    uint64_t id = 0, generation = 0, candidateGeneration = 0, nextCandidateGeneration = 0, lastSeq = 0;
    bool hasSeq = false, pending = false, frozen = false, sensitive = false;
    std::string raw, history, error, candidateSource = "dictionary";
    std::string candidateInput;
    size_t cursor = 0, selection = 0;
    int commitCursorBack = 0;
    std::vector<Selection> selected;
    std::vector<Candidate> candidates, dictionary;
    std::optional<CandidateSnapshot> previousCandidates;
    Cancellation cancelled;
    std::vector<std::string> pendingPrefixes;
    std::string selectedText() const {
        std::string text; for (const auto& item : selected) text += item.text; return text;
    }
};
struct Completion { std::string session; uint64_t generation; Cancellation cancelled; Result result; };
}
struct Engine::Impl {
    json config;
    std::unique_ptr<Dictionary> dictionary;
    std::shared_ptr<const LanguageModel> languageModel;
    std::unique_ptr<ShuangpinProfile> shuangpin;
    Core core;
    Worker worker;
    std::unordered_map<std::string, State> states;
    uint64_t nextId = 1;
    std::mutex completionsMutex;
    std::deque<Completion> completions;
    std::string dictionaryError, modelError, languageModelError;
    bool modelReady = false;
    std::filesystem::path learningFile;
    explicit Impl(json settings, Worker::Inference inference = {}) : config(std::move(settings)),
        worker(inference ? std::move(inference) : Worker::Inference([this](const Request& request) { return core.infer(request); })) {
        try {
            dictionary = std::make_unique<Dictionary>(std::filesystem::path(wide(config.value("dictionary_path", ""))));
            dictionary->setUserWordsExactOnly(config.value("user_words_exact_only", true));
            dictionary->setUserWordsEnabled(config.value("user_words_enabled", true));
            shuangpin = std::make_unique<ShuangpinProfile>(config.value("shuangpin_scheme", "xiaohe"), dictionary->syllables());
            if (!config.value("user_directory", "").empty()) {
                learningFile = std::filesystem::path(wide(config.value("user_directory", ""))) / "learning.json";
                dictionary->loadLearning(learningFile);
            }
        } catch (const std::exception& error) { dictionaryError = error.what(); }
        try {
            const auto path = config.value("language_model_path", "");
            if (path.empty()) languageModelError = "Set the official libime KenLM language model path in Euphony settings";
            else {
                languageModel = std::make_shared<LanguageModel>(std::filesystem::path(wide(path)));
                if (dictionary) dictionary->setLanguageModel(languageModel);
            }
        } catch (const std::exception& error) { languageModelError = error.what(); }
        if (config.value("model_enabled", true)) {
            if (config.value("model_directory", "").empty() || config.value("core_library", "").empty())
                modelError = "Set the local phono-core library and model package directory in Euphony settings";
            else {
                Request warmup; warmup.settings = coreSettings();
                const auto cancellation = warmup.cancelled;
                worker.submit(std::move(warmup), [this, cancellation](Result result) {
                    std::lock_guard lock(completionsMutex);
                    completions.push_back({{}, 0, cancellation, std::move(result)});
                });
            }
        }
    }
    ~Impl() {
        worker.stop();
        if (config.value("learning",true) && dictionary && !learningFile.empty()) {
            try { dictionary->saveLearning(learningFile); } catch (...) { }
        }
    }
    Settings coreSettings() const {
        auto engine = config.value("engine", json{{"schema_version", "1.0"}});
        engine["tokenizer"]["use_segmenter"] = config.value("input_mode", "pinyin") == "pinyin";
        return {config.value("model_directory", ""), config.value("core_library", ""), engine.dump(),
            config.value("context", json{{"schema_version", "1.0"}}).dump(), config.value("context_slots", 4)};
    }
    int pageSize() const { return std::clamp(config.value("page_size", 5), 3, 10); }
    size_t historyLimit() const { return static_cast<size_t>(std::clamp(config.value("history_characters", 256), 0, 8192)); }
    void cancel(State& state, bool freeze = false) {
        if (state.cancelled) state.cancelled->store(true);
        state.pending = false;
        if (freeze) state.frozen = true;
    }
    CandidateSnapshot candidateSnapshot(State& state) {
        return {state.candidateGeneration,state.selection,state.candidateSource,std::move(state.candidates)};
    }
    void candidateIdentity(State& state,const CandidateSnapshot& previous) {
        auto input=state.selectedText();input+='\0';input+=state.raw;
        if(input!=state.candidateInput || previous.candidates!=state.candidates) {
            if(input==state.candidateInput && !state.sensitive && !previous.candidates.empty())
                state.previousCandidates=previous;
            else state.previousCandidates.reset();
            state.candidateGeneration=++state.nextCandidateGeneration;state.candidateInput=std::move(input);
        }
    }
    void freezePresentedCandidates(const json& request) {
        if (request.value("version",0)!=1 || request.value("op","")!="key" ||
            !request.contains("candidate_generation") || request.value("ctrl",false) || request.value("alt",false)) return;
        const auto found=states.find(request.value("session",""));
        if(found==states.end())return;
        auto& state=found->second;
        if(state.candidates.empty() || state.raw.empty() ||
            (state.hasSeq && request.value("seq",uint64_t(0))<=state.lastSeq))return;
        const auto token=request.at("candidate_generation").get<uint64_t>();
        const auto* previous=state.previousCandidates && state.previousCandidates->generation==token ? &*state.previousCandidates : nullptr;
        if(token!=state.candidateGeneration && !previous)return;
        const auto& candidates=previous ? previous->candidates : state.candidates;
        const auto selection=previous ? previous->selection : state.selection;
        const auto key=request.value("key","");
        bool interaction=key=="Space" || key=="Up" || key=="Down" || key=="Tab" || key=="PageUp" ||
            key=="PageDown" || key=="-" || key=="=" || key=="_" || key=="+" || key=="Delete";
        if(key=="Select" || key=="Highlight") {
            const auto index=request.value("index",-1);
            interaction=index>=0 && static_cast<size_t>(index)<candidates.size();
        } else if(key.size()==1 && key[0]>='0' && key[0]<='9' && !request.value("shift",false)) {
            const auto slot=key[0]=='0'?9:key[0]-'1';
            interaction=(selection/pageSize())*pageSize()+slot<candidates.size();
        }
        // The user is operating the list already shown by TSF. Cancel queued
        // inference before draining it so a just-finished result cannot replace
        // that list between the click/key and its selection-token check.
        if(interaction) {
            if(previous) {
                // A poll can update the broker before its reply reaches the
                // TSF edit session. One prior list for this unchanged input
                // bridges that interval, including its highlighted page.
                state.candidates=previous->candidates;state.selection=previous->selection;
                state.candidateSource=previous->source;state.candidateGeneration=previous->generation;
                state.previousCandidates.reset();++state.generation;
            }
            cancel(state,true);
        }
    }
    void drain() {
        std::deque<Completion> finished;
        { std::lock_guard lock(completionsMutex); finished.swap(completions); }
        for (auto& completion : finished) {
            if (completion.cancelled->load()) continue;
            modelReady = completion.result.ready || modelReady;
            if (!completion.result.error.empty()) modelError = completion.result.error;
            else if (completion.result.ready) modelError.clear();
            if (completion.session.empty()) continue;
            auto found = states.find(completion.session);
            if (found == states.end()) continue;
            auto& state = found->second;
            if (state.generation != completion.generation || state.frozen || state.sensitive || state.raw.empty()) continue;
            state.pending = false;
            state.error = completion.result.error;
            auto previous=candidateSnapshot(state);state.candidates.clear();
            std::unordered_set<std::string> seen;
            for (const auto& candidate : state.dictionary)
                if (config.value("user_words_first", true) && candidate.user && candidate.preferred && seen.insert(candidate.commit).second)
                    state.candidates.push_back(candidate);
            for (const auto& decoded : completion.result.candidates) {
                auto presentation = presentCandidate(decoded, completion.result.invalid,
                    config.value("invalid_input", "pass") == "delete" ? InvalidInputPolicy::Delete : InvalidInputPolicy::PassThrough);
                if (!seen.insert(presentation.committed).second) continue;
                std::string text; for (const auto& piece : presentation.pieces) text += piece.text;
                state.candidates.push_back({text, presentation.committed, {}, true, state.raw.size(), std::move(presentation.pieces)});
            }
            for (const auto& candidate : state.dictionary)
                if (candidate.user && (candidate.consumed == state.raw.size() || utf8Tail(candidate.commit,1) != candidate.commit) &&
                    seen.insert(candidate.commit).second) state.candidates.push_back(candidate);
            for (const auto& candidate : state.dictionary)
                if (seen.insert(candidate.commit).second) state.candidates.push_back(candidate);
            state.candidateSource = !state.candidates.empty() && state.candidates.front().model ? "phono" : "dictionary";
            state.selection = 0;
            candidateIdentity(state,previous);
            ++state.generation;
        }
    }
    void synchronizeHistory(State& state, const json& request) {
        const bool sensitive = request.value("sensitive", false);
        if (sensitive != state.sensitive) {
            cancel(state);
            state.history.clear(); state.pendingPrefixes.clear();
            state.sensitive = sensitive;
            state.previousCandidates.reset();
            state.error.clear();
            auto previous=candidateSnapshot(state);state.candidates = state.dictionary;
            state.candidateSource="dictionary";
            candidateIdentity(state,previous);
            ++state.generation;
        }
        if (sensitive) { state.history.clear(); state.pendingPrefixes.clear(); return; }
        if (!config.value("use_surrounding", true) || !request.value("context_valid", false)) return;
        // The TSF host supplies only the prefix before the composition/caret.
        // An unchanged or partially acknowledged application prefix cannot
        // erase a commit which TSF has not yet reflected in GetText.
        auto prefix = utf8Tail(request.value("surrounding", ""), historyLimit());
        if (!state.pendingPrefixes.empty()) {
            if (prefix == state.history) state.pendingPrefixes.clear();
            else if (std::find(state.pendingPrefixes.begin(), state.pendingPrefixes.end(), prefix) != state.pendingPrefixes.end()) return;
            else state.pendingPrefixes.clear();
        }
        if (state.history != prefix) {
            state.history = std::move(prefix);
            // The next key/update resubmits inference with this document
            // prefix. poll handles this too so stale context cannot complete.
            cancel(state);
            ++state.generation;
        }
    }
    void record(State& state, const std::string& commit) {
        if (state.sensitive) { state.history.clear(); state.pendingPrefixes.clear(); return; }
        if (commit.empty()) return;
        if (state.pendingPrefixes.empty()) state.pendingPrefixes.push_back(state.history);
        state.history = utf8Tail(state.history + commit, historyLimit());
        state.pendingPrefixes.push_back(state.history);
        if (state.pendingPrefixes.size() > 64) state.pendingPrefixes.erase(state.pendingPrefixes.begin() + 1);
    }
    std::string normalizedPinyin(std::string raw) const {
        const auto normalization=config.value("engine",json::object()).value("tokenizer",json::object()).value("normalization",json::object());
        if(normalization.value("normalize_v_to_u",true))
            for(size_t i=1;i<raw.size();++i)
                if(raw[i]=='v' && std::string_view("jqxy").find(raw[i-1])!=std::string_view::npos)raw[i]='u';
        return raw;
    }
    std::vector<DictionaryCandidate> dictionaryCandidates(State& state, ShuangpinConversion* converted, bool* unigramOnly) {
        if (!dictionary || state.raw.empty() || !state.cursor) return {};
        auto raw = state.raw.substr(0, state.cursor);
        const auto history = state.history + state.selectedText();
        const bool preferUnigram = languageModel && config.value("prefer_unigram_single_syllable", true) &&
            history.empty() && state.selected.empty() && state.cursor == state.raw.size();
        if (config.value("input_mode", "pinyin") != "shuangpin" || !shuangpin) {
            raw = normalizedPinyin(std::move(raw));
            *unigramOnly = preferUnigram && dictionary->syllables().contains(raw);
            auto candidates = dictionary->lookup(raw, 64, history, *unigramOnly);
            // A syllable without a single-character entry cannot satisfy the
            // requested route; preserve the normal model path in that case.
            if (*unigramOnly && candidates.empty()) { *unigramOnly = false; return dictionary->lookup(raw, 64, history); }
            return candidates;
        }
        *converted = shuangpin->convert(raw);
        if (converted->tokens.empty()) return {};
        *unigramOnly = preferUnigram && converted->invalid.empty() && converted->tokens.size() == 1 &&
            converted->tokens.front().end - converted->tokens.front().begin == 2;
        // Preserve two-key ambiguities and resolve them against native words.
        std::vector<std::string> spellings{{}};
        for (const auto& token : converted->tokens) {
            std::vector<std::string> next;
            for (const auto& prefix : spellings)
                for (const auto& alternative : token.alternatives) {
                    next.push_back(prefix + (prefix.empty() ? "" : "'") + alternative);
                    if (next.size() >= 16) break;
                }
            if (next.size() > 16) next.resize(16);
            spellings = std::move(next);
        }
        std::vector<DictionaryCandidate> all;
        for (const auto& spelling : spellings) {
            const auto syllables = splitPinyin(spelling);
            for (auto candidate : dictionary->lookup(spelling, 64, history, *unigramOnly)) {
                size_t consumed = 0, accepted = 0;
                for (const auto& syllable : syllables) {
                    if (consumed + syllable.size() > candidate.consumed) break;
                    consumed += syllable.size() + (accepted ? 1 : 0); ++accepted;
                }
                // Whole or final-incomplete dictionary proposals consume their
                // corresponding original pair, not the expanded pinyin bytes.
                accepted = std::min(accepted, converted->tokens.size());
                if (candidate.complete) accepted = converted->tokens.size();
                if (!accepted) continue;
                candidate.consumed = candidate.complete ? raw.size() : converted->tokens[accepted - 1].end;
                candidate.complete = candidate.consumed == raw.size();
                all.push_back(std::move(candidate));
            }
        }
        std::stable_sort(all.begin(), all.end(), [this](const auto& a,const auto& b) {
            if (dictionary->priority(a) != dictionary->priority(b)) return dictionary->priority(a) > dictionary->priority(b);
            if (a.complete != b.complete) return a.complete;
            if (a.consumed != b.consumed) return a.consumed > b.consumed;
            return a.score > b.score;
        });
        std::unordered_set<std::string> seen;
        std::erase_if(all,[&](const auto& candidate){ return !seen.insert(candidate.text+"\t"+std::to_string(candidate.consumed)).second; });
        if (all.size() > 64) all.resize(64);
        if (*unigramOnly && all.empty()) {
            *unigramOnly = false;
            for (const auto& spelling : spellings)
                for (auto candidate : dictionary->lookup(spelling, 64, history)) {
                    candidate.consumed = raw.size(); candidate.complete = true; all.push_back(std::move(candidate));
                }
        }
        if (!all.empty()) preferShuangpinReading(*converted, all.front().pinyin);
        return all;
    }
    void update(const std::string& session, State& state, bool infer = true) {
        cancel(state);
        ++state.generation;
        auto previous=candidateSnapshot(state);
        state.frozen = false; state.selection = 0; state.error.clear();
        state.dictionary.clear(); state.candidates.clear();
        ShuangpinConversion converted;
        bool unigramOnly = false;
        for (const auto& native : dictionaryCandidates(state, &converted, &unigramOnly)) {
            auto invalid = converted.invalid;
            std::erase_if(invalid,[&](const auto& range){return range.end > native.consumed;});
            auto presentation = presentCandidate(native.text, invalid,
                config.value("invalid_input", "pass") == "delete" ? InvalidInputPolicy::Delete : InvalidInputPolicy::PassThrough);
            std::string text; for (const auto& piece : presentation.pieces) text += piece.text;
            state.dictionary.push_back({text, presentation.committed, native.pinyin, false,
                native.consumed, std::move(presentation.pieces),
                config.value("user_words_enabled", true) && dictionary && dictionary->frequency(native.pinyin,presentation.committed) != 0,
                dictionary && dictionary->priority(native) != 0});
        }
        if (state.dictionary.empty() && converted.tokens.empty() && !converted.invalid.empty()) {
            auto presentation = presentCandidate({},converted.invalid,
                config.value("invalid_input", "pass") == "delete" ? InvalidInputPolicy::Delete : InvalidInputPolicy::PassThrough);
            state.dictionary.push_back({state.raw,presentation.committed,{},false,state.raw.size(),std::move(presentation.pieces)});
        }
        state.candidates = state.dictionary;
        state.candidateSource = unigramOnly ? "unigram" : "dictionary";
        candidateIdentity(state,previous);
        if (!dictionaryError.empty()) state.error = dictionaryError;
        const auto maxChars = config.value("engine", json::object()).value("tokenizer", json::object()).value("max_pinyin_chars", 128);
        if (!infer || unigramOnly || !config.value("model_enabled", true) || state.raw.empty() || state.sensitive ||
            !state.selected.empty() || state.cursor != state.raw.size() || state.raw.size() > static_cast<size_t>(maxChars)) return;
        if (config.value("model_directory", "").empty() || config.value("core_library", "").empty()) { state.error = modelError; return; }
        Request request;
        request.contextId = state.id; request.pinyin = state.raw; request.history = state.history;
        request.settings = coreSettings();
        request.invalidPolicy = config.value("invalid_input", "pass") == "delete" ? InvalidInputPolicy::Delete : InvalidInputPolicy::PassThrough;
        if (config.value("input_mode", "pinyin") == "shuangpin" && shuangpin) {
            request.explicitSyllables = true;
            if (converted.tokens.empty()) converted = shuangpin->convert(state.raw);
            request.syllables = std::move(converted.tokens); request.invalid = std::move(converted.invalid);
        }
        state.cancelled = request.cancelled;
        state.pending = true;
        const auto generation = state.generation;
        const auto cancellation = request.cancelled;
        worker.submit(std::move(request), [this, session, generation, cancellation](Result result) {
            std::lock_guard lock(completionsMutex);
            completions.push_back({session, generation, cancellation, std::move(result)});
        });
    }
    void clear(State& state) {
        auto previous=candidateSnapshot(state);
        cancel(state); ++state.generation;
        state.raw.clear(); state.selected.clear(); state.cursor = state.selection = 0;
        state.dictionary.clear(); state.candidates.clear(); state.error.clear(); state.frozen = false;
        state.candidateSource = "dictionary";
        candidateIdentity(state,previous);
    }
    void commit(State& state, std::string& output, const std::string& text) {
        output += text;
        // Paired punctuation places the caret before the closing BMP mark.
        // Only text to the left of that caret belongs to model history.
        record(state,state.commitCursorBack == 1 && text.size() >= 3 ? text.substr(0,text.size()-3) : text);
        clear(state);
    }
    bool select(const std::string& session, State& state, size_t index, std::string& output) {
        if (index >= state.candidates.size()) return false;
        const auto candidate = state.candidates[index];
        cancel(state);
        if (candidate.model) { commit(state, output, state.selectedText() + candidate.commit); return true; }
        const auto consumed = std::min(candidate.consumed, state.raw.size());
        if (!consumed) return false;
        state.selected.push_back({state.raw.substr(0, consumed),candidate.commit,candidate.pinyin});
        state.raw.erase(0, consumed);
        if (!state.raw.empty() && state.raw.front() == '\'') {
            state.selected.back().source += '\''; state.raw.erase(0,1);
        }
        state.cursor = state.raw.size();
        if (state.raw.empty()) {
            const auto sentence=state.selectedText();
            if(config.value("learning",true) && !state.sensitive && dictionary) {
                std::string pinyin;
                for(const auto& selected:state.selected) {
                    if(selected.pinyin.empty()){pinyin.clear();break;}
                    if(!pinyin.empty())pinyin+='\'';
                    pinyin+=selected.pinyin;
                }
                if(!pinyin.empty())dictionary->learn(pinyin,sentence);
                if(!learningFile.empty()){try{dictionary->saveLearning(learningFile);}catch(...){}}
            }
            commit(state, output, sentence);
        }
        else update(session, state, false);
        return true;
    }
    void unselect(State& state) {
        if (state.selected.empty()) return;
        auto selected = std::move(state.selected.back()); state.selected.pop_back();
        state.raw.insert(0, selected.source); state.cursor = selected.source.size();
    }
    std::optional<std::string> punctuation(State& state, const std::string& key) {
        if (state.sensitive) return std::nullopt;
        const auto fullwidth = [&]() -> std::optional<std::string> {
            if (!config.value("full_width",false)) return std::nullopt;
            if (key == "Space" || key == " ") return "　";
            if (key.size()!=1 || key[0] < '!' || key[0] > '~') return std::nullopt;
            if (key[0]>='0' && key[0]<='9' && !config.value("full_width_digits",false)) return std::nullopt;
            if (std::string_view("~@#%&*-=+|/").find(key[0])!=std::string_view::npos && !config.value("full_width_symbols",false)) return std::nullopt;
            const uint32_t code = uint32_t(static_cast<unsigned char>(key[0])) + 0xfee0;
            return std::string{char(0xe0 | (code >> 12)),char(0x80 | ((code >> 6) & 0x3f)),char(0x80 | (code & 0x3f))};
        };
        if (config.value("full_width",false) && config.value("full_width_symbols",false) && key.size()==1 &&
            std::string_view("~@#%&*-=+|/").find(key[0])!=std::string_view::npos) return fullwidth();
        if (!config.value("chinese_punctuation",true)) return fullwidth();
        // Decimal numbers and URL/email separators keep ASCII punctuation.
        if ((key == "." || key == "," || key == ":") && !state.history.empty() &&
            state.history.back() >= '0' && state.history.back() <= '9') return fullwidth();
        static const std::unordered_map<std::string,std::string> pairs{{"(","（）"},{"[","【】"},{"<","《》"},{"{","『』"}};
        if (auto pair=pairs.find(key);pair!=pairs.end()) {state.commitCursorBack=1;return pair->second;}
        static const std::unordered_map<std::string,std::string> map{{",","，"},{".","。"},{"?","？"},{"!","！"},
            {";","；"},{":","："},{"(","（"},{")","）"},{"[","【"},{"]","】"},{"<","《"},{">","》"},
            {"\\","、"},{"$","¥"},{"^","……"},{"_","——"},{"/","、"},{"|","·"},{"{","『"},{"}","』"}};
        auto found = map.find(key);
        if (found != map.end()) {
            if (key=="$" && config.value("full_width",false) && config.value("full_width_symbols",false)) return "￥";
            return found->second;
        }
        if (key == "\"") {
            const auto left = state.history.rfind("“"), right = state.history.rfind("”");
            if (left != std::string::npos && (right == std::string::npos || left > right)) return "”";
            state.commitCursorBack=1; return "“”";
        }
        if (key == "'") {
            const auto left = state.history.rfind("‘"), right = state.history.rfind("’");
            if (left != std::string::npos && (right == std::string::npos || left > right)) return "’";
            state.commitCursorBack=1; return "‘’";
        }
        return fullwidth();
    }
    bool key(const std::string& session, State& state, const json& request, std::string& output) {
        const auto key = request.value("key", "");
        const bool shift = request.value("shift", false);
        if (request.value("ctrl", false) || request.value("alt", false)) {
            cancel(state,true); state.history.clear(); state.pendingPrefixes.clear(); return false;
        }
        if (key == "Select" || key == "Highlight") {
            if (!request.contains("candidate_generation") || request.at("candidate_generation").get<uint64_t>() != state.candidateGeneration) return false;
            const auto index = request.value("index", -1);
            if (index < 0 || static_cast<size_t>(index) >= state.candidates.size()) return false;
            if (key == "Select") return select(session,state,index,output);
            cancel(state,true); state.selection = static_cast<size_t>(index); ++state.generation; return true;
        }
        if (!state.raw.empty() && (key == "Tab" || key == "PageUp" || key == "PageDown" || key == "-" || key == "=" || key == "_" || key == "+")) {
            cancel(state,true);
            const bool previous = key == "PageUp" || key == "-" || key == "_" || (key == "Tab" && shift);
            const auto count = state.candidates.size();
            const auto page = state.selection / pageSize();
            if (count) {
                const auto pages = (count + pageSize() - 1) / pageSize();
                state.selection = (previous ? (page + pages - 1) % pages : (page + 1) % pages) * pageSize();
            }
            ++state.generation; return true;
        }
        const bool letter = key.size() == 1 && key[0] >= 'a' && key[0] <= 'z' && !shift;
        const bool separator = key == "'" && !state.raw.empty();
        const bool semicolon = key == ";" && !state.raw.empty() && config.value("input_mode", "pinyin") == "shuangpin" && shuangpin && shuangpin->usesSemicolon();
        if (letter || separator || semicolon) {
            const auto maxChars = config.value("engine", json::object()).value("tokenizer", json::object()).value("max_pinyin_chars",128);
            if (state.raw.size() >= static_cast<size_t>(maxChars)) return true;
            state.raw.insert(state.cursor,key); state.cursor += key.size(); update(session,state); return true;
        }
        if (state.raw.empty()) {
            if (auto converted = punctuation(state,key)) { commit(state,output,*converted); return true; }
            // Passing an ordinary application key invalidates local prediction.
            state.history.clear(); state.pendingPrefixes.clear(); return false;
        }
        if (key.size()==1 && key[0]>='0' && key[0]<='9' && !shift) {
            const auto slot = key[0]=='0' ? 9 : key[0]-'1';
            return select(session,state,(state.selection/pageSize())*pageSize()+slot,output);
        }
        if (key == "Space") {
            if (!select(session,state,state.selection,output)) commit(state,output,state.selectedText()+state.raw);
            return true;
        }
        if (key == "Enter") { commit(state,output,state.selectedText()+state.raw); return true; }
        if (key == "Escape") { clear(state); return true; }
        if (key == "Delete" && state.selection < state.candidates.size() && state.candidates[state.selection].user) {
            if (request.contains("candidate_generation") && request.at("candidate_generation").get<uint64_t>() != state.candidateGeneration) return false;
            const auto candidate = state.candidates[state.selection];
            dictionary->forget(candidate.pinyin,candidate.commit);
            if (!learningFile.empty()) dictionary->saveLearning(learningFile);
            update(session,state);
            return true;
        }
        if (key == "Up" || key == "Down") {
            cancel(state,true);
            if (!state.candidates.empty()) state.selection = (state.selection + state.candidates.size() + (key=="Down" ? 1 : -1)) % state.candidates.size();
            ++state.generation; return true;
        }
        if (key == "Backspace") {
            if (!state.cursor && !state.selected.empty()) unselect(state);
            else if (state.cursor) state.raw.erase(--state.cursor,1);
        } else if (key == "Delete") {
            if (state.cursor < state.raw.size()) state.raw.erase(state.cursor,1);
        } else if (key == "Left") {
            if (!state.cursor && !state.selected.empty()) unselect(state);
            else if (state.cursor) --state.cursor;
        } else if (key == "Right") state.cursor = std::min(state.raw.size(),state.cursor+1);
        else if (key == "Home") state.cursor = 0;
        else if (key == "End") state.cursor = state.raw.size();
        else {
            if (!select(session,state,state.selection,output)) commit(state,output,state.selectedText()+state.raw);
            if (!state.raw.empty()) return true;
            if (auto converted = punctuation(state,key)) { commit(state,output,*converted); return true; }
            state.history.clear(); state.pendingPrefixes.clear(); return false;
        }
        if (state.raw.empty() && state.selected.empty()) clear(state);
        else update(session,state);
        return true;
    }
    json response(const std::string& session,uint64_t seq,const State& state,bool eaten,const std::string& commit) const {
        const auto prefix = state.selectedText();
        json candidates = json::array();
        for (const auto& candidate : state.candidates) {
            json pieces = json::array();
            for (const auto& piece : candidate.pieces) pieces.push_back({{"text",piece.text},{"deleted",piece.deleted}});
            candidates.push_back({{"text",candidate.text},{"commit",prefix+candidate.commit},{"model",candidate.model},
                {"user",candidate.user},
                {"continues",!candidate.model && candidate.consumed < state.raw.size()},{"pieces",std::move(pieces)}});
        }
        return {{"version",1},{"session",session},{"seq",seq},{"eaten",eaten},{"preedit",prefix+state.raw},
            {"cursor",utf16Length(prefix+state.raw.substr(0,state.cursor))},{"commit",commit},
            {"commit_cursor_back",state.commitCursorBack},{"candidates",std::move(candidates)},
            {"selection",state.selection},{"page",state.selection/pageSize()},{"page_size",pageSize()},
            {"theme",config.value("theme","system")},
            {"background_color",config.value("background_color","#FAFBFF")},
            {"selected_candidate_color",config.value("selected_candidate_color","#E2EAFF")},
            {"dark_background_color",config.value("dark_background_color","#282828")},
            {"dark_selected_candidate_color",config.value("dark_selected_candidate_color","#183B66")},
            {"dark_model_candidate_color",config.value("dark_model_candidate_color","#204532")},
            {"dark_user_candidate_color",config.value("dark_user_candidate_color","#442D50")},
            {"show_preedit",config.value("show_preedit",false)},
            {"candidate_layout",config.value("candidate_layout", "vertical")},
            {"model_candidate_color",config.value("model_candidate_color", "#DFF2E5")},
            {"user_candidate_color",config.value("user_candidate_color", "#FBE4EC")},
            {"generation",state.generation},{"candidate_generation",state.candidateGeneration},{"candidate_source",state.candidateSource},
            {"pending",state.pending},{"error",state.error}};
    }
};
Engine::Engine(const json& config) : impl_(std::make_unique<Impl>(config)) { }
Engine::Engine(const json& config, std::function<Result(const Request&)> inference)
    : impl_(std::make_unique<Impl>(config,std::move(inference))) { }
Engine::~Engine() = default;
json Engine::handle(const json& request) {
    auto& impl = *impl_;
    impl.freezePresentedCandidates(request);
    impl.drain();
    if (request.value("op", "") == "status")
        return {{"version",1},{"model_ready",impl.modelReady},{"dictionary_ready",bool(impl.dictionary)},
            {"model_error",impl.modelError},{"dictionary_error",impl.dictionaryError},
            {"language_model_ready",bool(impl.languageModel)},{"language_model_error",impl.languageModelError},
            {"language_model_order",impl.languageModel ? impl.languageModel->order() : 0},
            {"dictionary_entries",impl.dictionary ? impl.dictionary->size() : 0}};
    const auto session = request.value("session", "");
    const auto seq = request.value("seq", uint64_t(0));
    auto& state = impl.states[session];
    state.commitCursorBack = 0;
    if (!state.id) state.id = impl.nextId++;
    if (request.value("version", 0) != 1 || session.empty() || session.size() > 256) {
        auto answer = impl.response(session,seq,state,false,{}); answer["error"] = "Invalid engine protocol request"; return answer;
    }
    if (state.hasSeq && seq <= state.lastSeq) {
        auto answer = impl.response(session,seq,state,false,{}); answer["error"] = "Out-of-order or repeated request"; return answer;
    }
    state.hasSeq = true; state.lastSeq = seq;
    const auto beforeHistory = state.history;
    impl.synchronizeHistory(state,request);
    std::string commit;
    bool eaten = false;
    const auto op = request.value("op", "");
    if (op == "key") eaten = impl.key(session,state,request,commit);
    else if (op == "reset" || op == "close") {
        impl.clear(state); state.history.clear(); state.pendingPrefixes.clear();
    } else if (op == "poll") {
        if (beforeHistory != state.history && !state.raw.empty() && !state.frozen) impl.update(session,state);
    } else state.error = "Unsupported engine operation";
    auto answer = impl.response(session,seq,state,eaten,commit);
    if (op == "close") impl.states.erase(session);
    return answer;
}
}

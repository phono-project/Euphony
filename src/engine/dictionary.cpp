#include "dictionary.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <mutex>
#include <windows.h>

namespace euphony {
std::vector<std::string> splitPinyin(std::string_view text) {
    std::vector<std::string> result;
    for (size_t start = 0; start < text.size();) {
        auto end = text.find('\'', start);
        if (end == text.npos) end = text.size();
        if (end > start) result.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return result;
}
std::string utf8Tail(std::string_view text, size_t count) {
    size_t start = text.size();
    while (count-- && start) {
        --start;
        while (start && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) --start;
    }
    return std::string(text.substr(start));
}
size_t utf16Length(std::string_view text) {
    size_t count = 0;
    for (size_t i = 0; i < text.size();) {
        auto c = static_cast<unsigned char>(text[i]);
        size_t bytes = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        count += bytes == 4 ? 2 : 1;
        i += std::min(bytes, text.size() - i);
    }
    return count;
}
void Dictionary::insert(std::vector<Node>& trie, std::string_view key, uint32_t entry) {
    uint32_t node = 0;
    for (char c : key) {
        auto it = trie[node].next.find(c);
        if (it != trie[node].next.end()) { node = it->second; continue; }
        const auto next = static_cast<uint32_t>(trie.size());
        trie[node].next.emplace(c, next);
        trie.emplace_back();
        node = next;
    }
    trie[node].entries.push_back(entry);
}
Dictionary::Dictionary(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open libime dictionary: " + path.string());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line.front() == '#') continue;
        std::istringstream fields(line);
        Entry entry{};
        if (!(fields >> std::quoted(entry.text) >> entry.pinyin)) continue;
        if (!(fields >> entry.weight)) entry.weight = 0;
        if (!std::isfinite(entry.weight) || entry.text.empty()) continue;
        auto syllables = splitPinyin(entry.pinyin);
        if (syllables.empty()) continue;
        std::string flat, initials;
        bool valid = true;
        for (auto& syllable : syllables) {
            for (auto& c : syllable) {
                if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                if ((c < 'a' || c > 'z')) valid = false;
            }
            if (!valid) break;
            flat += syllable;
            initials += syllable[0];
            syllables_.insert(syllable);
        }
        if (!valid) continue;
        entry.pinyin.clear();
        for (auto& syllable : syllables) {
            if (!entry.pinyin.empty()) entry.pinyin += '\'';
            entry.pinyin += syllable;
        }
        const auto id = static_cast<uint32_t>(entries_.size());
        entries_.push_back(std::move(entry));
        insert(full_, flat, id);
        insert(initials_, initials, id);
    }
    if (entries_.empty()) throw std::runtime_error("libime dictionary contains no usable entries");
}
double Dictionary::rank(const Entry& entry) const {
    auto it = learned_.find(entry.pinyin + "\t" + entry.text);
    // libime text weights are log10 scores. User selections supply a bounded
    // ranking bonus without persisting document history or model proposals.
    return entry.weight + (!userWordsEnabled_ || it == learned_.end() ? 0.0 : 2.0 * std::log1p(it->second));
}
std::vector<DictionaryCandidate> Dictionary::matches(std::string_view raw, bool prefixes, size_t limit,
    const LanguageModel::State* context, bool unigramOnly) const {
    std::string normalized;
    std::vector<size_t> sourceEnd;
    std::vector<size_t> boundaries;
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '\'' || c == ' ') { if (!normalized.empty()) boundaries.push_back(normalized.size()); continue; }
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c < 'a' || c > 'z') break;
        normalized += c;
        sourceEnd.push_back(i + 1);
    }
    if (normalized.empty()) return {};
    std::vector<DictionaryCandidate> results;
    auto accept = [&](uint32_t id, size_t consumed, bool complete, bool abbreviated, bool completing = false) {
        const auto& entry = entries_[id];
        if (entry.userOnly && (!userWordsEnabled_ || !frequency(entry.pinyin,entry.text))) return;
        if (unigramOnly && (abbreviated || completing || !complete || entry.pinyin != normalized ||
            entry.pinyin.find('\'') != std::string::npos || utf8Tail(entry.text, 1) != entry.text)) return;
        if (completing && !abbreviated) {
            const auto tokens=splitPinyin(entry.pinyin);
            size_t total=0;for(const auto& token:tokens)total+=token.size();
            // A completion can extend only the last syllable of this word.
            // niha may propose ni'hao, but cannot invent ni'hao'ma.
            if(tokens.empty() || consumed<=total-tokens.back().size() || consumed>=total)return;
        }
        if (!abbreviated && !boundaries.empty()) {
            std::unordered_set<size_t> syllableEnds;
            size_t pos = 0;
            for (const auto& token : splitPinyin(entry.pinyin)) { pos += token.size(); syllableEnds.insert(pos); }
            for (auto boundary : boundaries) if (boundary < consumed && !syllableEnds.contains(boundary)) return;
        }
        double score = rank(entry);
        if (languageModel_) {
            if (unigramOnly) score = languageModel_->unigram(entry.text);
            else {
                LanguageModel::State next;
                score += context ? languageModel_->scoreText(*context, entry.text, next) : languageModel_->unigram(entry.text);
            }
        }
        results.push_back({entry.text, entry.pinyin, sourceEnd[consumed - 1],
            score - (abbreviated ? 8.0 : 0.0) - (completing ? 5.0 : 0.0) - 4.0 * splitPinyin(entry.pinyin).size(), complete, completing || abbreviated});
    };
    auto visit = [&](const std::vector<Node>& trie, bool abbreviated) {
        uint32_t node = 0;
        size_t matched = 0;
        for (char c : normalized) {
            auto it = trie[node].next.find(c);
            if (it == trie[node].next.end()) break;
            node = it->second;
            ++matched;
            if (prefixes || matched == normalized.size())
                for (auto id : trie[node].entries) accept(id, matched, matched == normalized.size(), abbreviated);
        }
        // Complete the final syllable while typing, but never consume bytes the
        // user did not enter. Bound traversal of very short prefixes.
        if (matched == normalized.size()) {
            std::vector<std::pair<uint32_t, int>> todo{{node, 0}};
            size_t visited = 0;
            while (!todo.empty() && ++visited < 12000) {
                auto [current, depth] = todo.back(); todo.pop_back();
                if (depth > 0)
                    for (auto id : trie[current].entries) {
                        const auto syllableCount = splitPinyin(entries_[id].pinyin).size();
                        // A completion extends one unfinished syllable. Avoid
                        // displaying an entire unrelated phrase for one letter.
                        if (abbreviated || syllableCount == 1 || normalized.size() > 2)
                            accept(id, matched, true, abbreviated, true);
                    }
                if (depth < (abbreviated ? 0 : 4))
                    for (const auto& [c, next] : trie[current].next) { (void)c; todo.emplace_back(next, depth + 1); }
            }
        }
    };
    visit(full_, false);
    if (normalized.size() <= 12 && boundaries.empty()) visit(initials_, true);
    std::stable_sort(results.begin(), results.end(), [this](const auto& a, const auto& b) {
        if (priority(a) != priority(b)) return priority(a) > priority(b);
        if (a.complete != b.complete) return a.complete;
        if (a.consumed != b.consumed) return a.consumed > b.consumed;
        return a.score > b.score;
    });
    std::unordered_set<std::string> seen;
    std::erase_if(results, [&](const auto& c) {
        return !seen.insert(c.text + "\t" + (userWordsEnabled_ && frequency(c.pinyin,c.text) ? c.pinyin : std::to_string(c.consumed))).second;
    });
    if (limit && results.size() > limit) results.resize(limit);
    return results;
}
std::vector<DictionaryCandidate> Dictionary::lookup(std::string_view raw, size_t limit,
    std::string_view history, bool unigramOnly) const {
    auto context = languageModel_ ? languageModel_->contextState(history) : LanguageModel::State{};
    // Prefer a path of complete syllables, with an unfinished final syllable.
    // Short input keeps alternate readings; longer input uses one global path.
    // Like libime's longest-match parser, a complete syllable beats initials.
    std::vector<size_t> nextBoundary(raw.size()+1, raw.size()+1);
    std::vector<int> cost(raw.size()+1, 100000);
    cost.back()=0;
    for (size_t pos=raw.size(); pos-->0;) {
        if (raw[pos]=='\'' || raw[pos]==' ') { cost[pos]=cost[pos+1]; nextBoundary[pos]=pos+1; continue; }
        for (size_t len=std::min(size_t(6),raw.size()-pos);len>0;--len) {
            std::string token(raw.substr(pos,len));
            for (auto& c:token) if(c>='A' && c<='Z') c+='a'-'A';
            const bool full=syllables_.contains(token);
            const bool partial=!full && pos+len==raw.size() &&
                std::any_of(syllables_.begin(),syllables_.end(),[&](const auto& syl){return syl.starts_with(token);});
            const int candidate=cost[pos+len]+1+(partial?2:0);
            if ((full || partial) && candidate<cost[pos]) { cost[pos]=candidate; nextBoundary[pos]=pos+len; }
        }
    }
    const bool constrained=raw.size()>6 && cost[0]<100000;
    std::vector<bool> boundary(raw.size()+1,false);
    if (constrained) for(size_t pos=0;pos<raw.size();pos=nextBoundary[pos]) { boundary[pos]=true; boundary[nextBoundary[pos]]=true; }
    auto compatible=[&](const DictionaryCandidate& word,size_t pos) {
        if (!constrained) return true;
        const auto end=pos+word.consumed;
        if (end>raw.size() || !boundary[pos] || !boundary[end]) return false;
        size_t source=pos;
        for(const auto& token:splitPinyin(word.pinyin)) {
            for(char c:token) {
                if(source==end) return end==raw.size(); // final-syllable completion
                char entered=raw[source++];
                if(entered>='A' && entered<='Z') entered+='a'-'A';
                if(c!=entered) return false; // no initials inside full pinyin
            }
            if(!boundary[source]) return false;
            while(source<end && (raw[source]=='\'' || raw[source]==' ')) ++source;
        }
        return source==end;
    };
    auto result = matches(raw, true, constrained ? 0 : limit, languageModel_ ? &context : nullptr, unigramOnly);
    std::erase_if(result,[&](const auto& word){return !compatible(word,0);});
    if (unigramOnly) return result;
    // Compose dictionary words when there is no whole phrase in the lexicon.
    // A small beam gives an immediate fallback while the model runs.
    if (!languageModel_ && !result.empty() && result.front().complete) {
        if (result.size()>limit) result.resize(limit);
        return result;
    }
    struct Beam { std::string text, pinyin; double score; size_t words; LanguageModel::State state; bool predictive = false; };
    std::vector<std::vector<Beam>> beams(raw.size() + 1);
    beams[0].push_back({{}, {}, 0, 0, context});
    for (size_t pos = 0; pos < raw.size(); ++pos) {
        if (beams[pos].empty()) continue;
        if (raw[pos] == '\'') { beams[pos + 1] = beams[pos]; continue; }
        auto words = matches(raw.substr(pos), true, (languageModel_ || constrained) ? 0 : 12);
        std::erase_if(words,[&](const auto& word){return !compatible(word,pos);});
        // Preserve competing word lengths. Truncating all words together can
        // remove single-character paths before the LM can compare a sentence.
        std::unordered_map<size_t, size_t> perEnd;
        for (const auto& word : words) {
            if (languageModel_ && perEnd[word.consumed]++ >= 12) continue;
            const auto end = pos + word.consumed;
            if (end > raw.size() || !word.consumed) continue;
            for (const auto& previous : beams[pos]) {
                auto score = word.score;
                auto next = previous.state;
                if (languageModel_) {
                    // matches used an independent unigram for pruning; replace
                    // it with the probability conditioned on this beam's words.
                    score += languageModel_->scoreText(previous.state, word.text, next) - languageModel_->unigram(word.text);
                }
                beams[end].push_back({previous.text + word.text,
                    previous.pinyin + (previous.pinyin.empty() ? "" : "'") + word.pinyin,
                    previous.score + score - (languageModel_ ? 0.0 : 0.5), previous.words + 1, std::move(next),
                    previous.predictive || word.predictive});
            }
            auto& target = beams[end];
            std::stable_sort(target.begin(), target.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
            if (target.size() > (languageModel_ ? 12 : 4)) target.resize(languageModel_ ? 12 : 4);
        }
    }
    std::vector<DictionaryCandidate> combined;
    for (const auto& beam : beams.back())
        if (beam.words > 1) combined.push_back({beam.text, beam.pinyin, raw.size(), beam.score, true, beam.predictive});
    result.insert(result.begin(),combined.begin(),combined.end());
    std::stable_sort(result.begin(), result.end(), [this](const auto& a, const auto& b) {
        if (priority(a) != priority(b)) return priority(a) > priority(b);
        if (!languageModel_) return false;
        if (a.complete != b.complete) return a.complete;
        if (a.consumed != b.consumed) return a.consumed > b.consumed;
        return a.score > b.score;
    });
    std::unordered_set<std::string> seen;
    std::erase_if(result,[&](const auto& candidate){
        return !seen.insert(candidate.text+"\t"+(userWordsEnabled_ && frequency(candidate.pinyin,candidate.text) ? candidate.pinyin : std::to_string(candidate.consumed))).second;
    });
    if (result.size() > limit) result.resize(limit);
    return result;
}
void Dictionary::learn(std::string_view pinyin, std::string_view text) {
    auto& count = learned_[std::string(pinyin) + "\t" + std::string(text)];
    count = std::min(count + 1, uint32_t(100000));
    addLearnedEntry(pinyin,text);
}
uint32_t Dictionary::frequency(std::string_view pinyin, std::string_view text) const {
    const auto it = learned_.find(std::string(pinyin) + "\t" + std::string(text));
    return it == learned_.end() ? 0 : it->second;
}
uint32_t Dictionary::priority(const DictionaryCandidate& candidate) const {
    if (!userWordsEnabled_) return 0;
    if (userWordsExactOnly_ && (!candidate.complete || candidate.predictive)) return 0;
    if (!candidate.complete && utf8Tail(candidate.text,1) == candidate.text) return 0;
    return frequency(candidate.pinyin,candidate.text);
}
void Dictionary::forget(std::string_view pinyin, std::string_view text) {
    learned_.erase(std::string(pinyin) + "\t" + std::string(text));
}
void Dictionary::addLearnedEntry(std::string_view pinyin,std::string_view text) {
    if (text.empty() || pinyin.empty()) return;
    std::string flat,initials;
    for(const auto& syllable:splitPinyin(pinyin)) {
        if (!syllables_.contains(syllable)) return;
        flat+=syllable;initials+=syllable[0];
    }
    uint32_t node=0;
    bool found=true;
    for(char c:flat) {
        auto child=full_[node].next.find(c);
        if(child==full_[node].next.end()){found=false;break;}
        node=child->second;
    }
    if(found)
        for(auto id:full_[node].entries)
            if(entries_[id].pinyin==pinyin && entries_[id].text==text)return;
    const auto id=static_cast<uint32_t>(entries_.size());
    entries_.push_back({std::string(text),std::string(pinyin),0.0,true});
    insert(full_,flat,id);insert(initials_,initials,id);
}
void Dictionary::loadLearning(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return;
    try {
        learned_ = nlohmann::json::parse(in).at("selections").get<decltype(learned_)>(); saved_ = learned_;
        for(const auto& [key,count]:learned_) {
            (void)count;
            const auto separator=key.find('\t');
            if(separator!=std::string::npos)addLearnedEntry(std::string_view(key).substr(0,separator),std::string_view(key).substr(separator+1));
        }
    }
    catch (const nlohmann::json::exception&) { learned_.clear(); }
}
void Dictionary::saveLearning(const std::filesystem::path& file) const {
    if (file.empty()) return;
    static std::mutex persistenceMutex;
    std::lock_guard lock(persistenceMutex);
    std::filesystem::create_directories(file.parent_path());
    auto temporary = file; temporary += ".tmp";
    std::unordered_map<std::string, uint32_t> merged;
    // Configuration updates can keep an older engine alive until its active
    // compositions finish. Preserve selections written by either instance.
    try {
        std::ifstream existing(file);
        if (existing) {
            auto stored = nlohmann::json::parse(existing).at("selections").get<decltype(learned_)>();
            merged = std::move(stored);
        }
    } catch (const nlohmann::json::exception&) { }
    for (const auto& [key,count] : saved_)
        if (!learned_.contains(key)) merged.erase(key);
    for (const auto& [key,count] : learned_) {
        const auto previous = saved_.find(key);
        const auto baseline = previous == saved_.end() ? 0 : previous->second;
        const auto delta = count > baseline ? count - baseline : 0;
        if (!delta) continue;
        merged[key] = static_cast<uint32_t>(std::min(uint64_t(100000),uint64_t(merged[key])+delta));
    }
    { std::ofstream out(temporary, std::ios::trunc); if (!out) return;
      out << nlohmann::json{{"schema_version", "1.0"}, {"selections", merged}}.dump(2);
      if (!out) return; }
    if (!MoveFileExW(temporary.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error=GetLastError(); DeleteFileW(temporary.c_str());
        throw std::runtime_error("Cannot persist dictionary selections (Windows error "+std::to_string(error)+")");
    }
    saved_ = learned_;
}
}

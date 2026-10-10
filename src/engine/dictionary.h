#pragma once
#include "language_model.h"
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace euphony {
struct DictionaryCandidate {
    std::string text;
    std::string pinyin;
    size_t consumed = 0; // bytes of original composition
    double score = 0;
    bool complete = false;
    bool predictive = false; // the final syllable is not completely entered
};
class Dictionary {
public:
    explicit Dictionary(const std::filesystem::path& path);
    std::vector<DictionaryCandidate> lookup(std::string_view raw, size_t limit = 64,
        std::string_view history = {}, bool unigramOnly = false) const;
    void setLanguageModel(std::shared_ptr<const LanguageModel> model) { languageModel_ = std::move(model); }
    const std::unordered_set<std::string>& syllables() const { return syllables_; }
    size_t size() const { return entries_.size(); }
    void learn(std::string_view pinyin, std::string_view text);
    void loadLearning(const std::filesystem::path& file);
    void saveLearning(const std::filesystem::path& file) const;
private:
    struct Entry { std::string text, pinyin; double weight; };
    struct Node { std::unordered_map<char, uint32_t> next; std::vector<uint32_t> entries; };
    void insert(std::vector<Node>& trie, std::string_view key, uint32_t entry);
    std::vector<DictionaryCandidate> matches(std::string_view raw, bool prefixes, size_t limit,
        const LanguageModel::State* context = nullptr, bool unigramOnly = false) const;
    double rank(const Entry&) const;
    void addLearnedEntry(std::string_view pinyin,std::string_view text);
    std::vector<Entry> entries_;
    std::vector<Node> full_{1}, initials_{1};
    std::unordered_set<std::string> syllables_;
    std::unordered_map<std::string, uint32_t> learned_;
    mutable std::unordered_map<std::string, uint32_t> saved_;
    std::shared_ptr<const LanguageModel> languageModel_;
};
std::vector<std::string> splitPinyin(std::string_view);
std::string utf8Tail(std::string_view text, size_t characters);
size_t utf16Length(std::string_view text);
}

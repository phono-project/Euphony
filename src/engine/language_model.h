#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace euphony {
// Owns the official libime KenLM data; no libime or Fcitx runtime is needed.
class LanguageModel {
public:
    // KenLM's virtual interface writes a POD state. uint64_t storage keeps it
    // aligned while leaving KenLM headers out of the engine's public interface.
    struct State { std::vector<uint64_t> storage; };
    explicit LanguageModel(const std::filesystem::path& path);
    ~LanguageModel();
    State nullState() const;
    State contextState(std::string_view text) const;
    bool known(std::string_view word) const;
    double unigram(std::string_view word) const;
    double score(const State& input, std::string_view word, State& output) const;
    // A learned phrase may not be a single LM token. Decode its known words
    // instead of assigning the entire phrase one unknown-word probability.
    double scoreText(const State& input, std::string_view text, State& output) const;
    unsigned order() const;
private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
}

#include "language_model.h"
#include "dictionary.h"
#include <lm/config.hh>
#include <lm/model.hh>
#include <lm/virtual_interface.hh>
#include <util/string_piece.hh>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <windows.h>

namespace euphony {
namespace {
std::string nativeFilename(const std::filesystem::path& file) {
    // Upstream KenLM uses the narrow Windows CRT file API. Prefer an ASCII
    // short name, then a lossless native code-page name; never open a different
    // path after replacement characters have been substituted.
    auto filename = std::filesystem::absolute(file).native();
    const auto required = GetShortPathNameW(filename.c_str(), nullptr, 0);
    if (required) {
        std::wstring shortName(required, L'\0');
        const auto length = GetShortPathNameW(filename.c_str(), shortName.data(), required);
        if (length && length < required) { shortName.resize(length); filename = std::move(shortName); }
    }
    const auto codepage = GetACP();
    BOOL substituted = FALSE;
    auto used = codepage == CP_UTF8 ? nullptr : &substituted;
    const DWORD flags = codepage == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    const auto size = WideCharToMultiByte(codepage, flags, filename.data(), static_cast<int>(filename.size()),
        nullptr, 0, nullptr, used);
    if (!size || substituted) throw std::runtime_error("KenLM cannot open this path; choose an ASCII model path");
    std::string result(size, '\0');
    if (!WideCharToMultiByte(codepage, flags, filename.data(), static_cast<int>(filename.size()),
        result.data(), size, nullptr, used) || substituted)
        throw std::runtime_error("KenLM cannot open this path; choose an ASCII model path");
    return result;
}
std::vector<size_t> utf8Boundaries(std::string_view text) {
    std::vector<size_t> boundaries{0};
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        const size_t bytes = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        i += std::min(bytes, text.size() - i); boundaries.push_back(i);
    }
    return boundaries;
}
bool sentenceBoundary(std::string_view character) {
    return character == "\n" || character == "\r" || character == "." || character == "!" || character == "?" ||
        character == "。" || character == "！" || character == "？";
}
}
struct LanguageModel::Impl {
    std::unique_ptr<lm::base::Model> model;
    explicit Impl(const std::filesystem::path& path) {
        if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("Cannot open libime KenLM language model");
        lm::ngram::Config settings;
        settings.show_progress = false; settings.messages = nullptr;
        settings.sentence_marker_missing = lm::SILENT;
        settings.arpa_complain = lm::ngram::Config::NONE;
        model.reset(lm::ngram::LoadVirtual(nativeFilename(path).c_str(), settings));
    }
    State allocate() const { return {std::vector<uint64_t>((model->StateSize() + sizeof(uint64_t) - 1) / sizeof(uint64_t))}; }
};
LanguageModel::LanguageModel(const std::filesystem::path& path) {
    // Retained engines can overlap during configuration changes. Share the
    // read-only mapping instead of loading another copy of a large model.
    static std::mutex mutex;
    static std::unordered_map<std::wstring, std::weak_ptr<const Impl>> models;
    const auto canonical = std::filesystem::weakly_canonical(path).native();
    std::lock_guard lock(mutex);
    impl_ = models[canonical].lock();
    if (!impl_) { impl_ = std::make_shared<Impl>(path); models[canonical] = impl_; }
}
LanguageModel::~LanguageModel() = default;
LanguageModel::State LanguageModel::nullState() const {
    auto state = impl_->allocate(); impl_->model->NullContextWrite(state.storage.data()); return state;
}
unsigned LanguageModel::order() const { return impl_->model->Order(); }
bool LanguageModel::known(std::string_view word) const {
    const auto& vocabulary = impl_->model->BaseVocabulary();
    return vocabulary.Index(StringPiece(word.data(), word.size())) != vocabulary.NotFound();
}
double LanguageModel::score(const State& input, std::string_view word, State& output) const {
    const auto& vocabulary = impl_->model->BaseVocabulary();
    const auto index = vocabulary.Index(StringPiece(word.data(), word.size()));
    auto next = impl_->allocate();
    const double result = impl_->model->BaseScore(input.storage.data(), index, next.storage.data());
    output = std::move(next);
    // Match libime's additional unknown-token penalty, 1 / 60,000,000.
    return result + (index == vocabulary.NotFound() ? std::log10(1.0 / 60000000.0) : 0.0);
}
double LanguageModel::unigram(std::string_view word) const {
    auto input = nullState(); State output; return score(input, word, output);
}
double LanguageModel::scoreText(const State& input, std::string_view text, State& output) const {
    if (text.empty()) { output = input; return 0; }
    if (known(text)) return score(input, text, output);
    const auto positions = utf8Boundaries(text);
    struct Beam { State state; double score; };
    std::vector<std::vector<Beam>> beams(positions.size());
    beams[0].push_back({input, 0});
    for (size_t pos = 0; pos + 1 < positions.size(); ++pos) {
        if (beams[pos].empty()) continue;
        for (size_t end = pos + 1; end < positions.size() && end <= pos + 12; ++end) {
            const auto word = text.substr(positions[pos], positions[end] - positions[pos]);
            if (end != pos + 1 && !known(word)) continue;
            auto& target = beams[end];
            for (const auto& previous : beams[pos]) {
                State next;
                const auto probability = score(previous.state, word, next);
                target.push_back({std::move(next), previous.score + probability});
            }
            std::stable_sort(target.begin(), target.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
            if (target.size() > 6) target.resize(6);
        }
    }
    output = beams.back().front().state;
    return beams.back().front().score;
}
LanguageModel::State LanguageModel::contextState(std::string_view original) const {
    // The LM needs only a short suffix to recover its n-gram state. History is
    // not stored by this object and never travels to another session.
    const auto text = utf8Tail(original, 64);
    const auto positions = utf8Boundaries(text);
    size_t start = 0;
    bool sentence = original.empty();
    for (size_t pos = 0; pos + 1 < positions.size(); ++pos)
        if (sentenceBoundary(std::string_view(text).substr(positions[pos], positions[pos + 1] - positions[pos]))) {
            start = positions[pos + 1]; sentence = true;
        }
    auto state = impl_->allocate();
    if (sentence) impl_->model->BeginSentenceWrite(state.storage.data());
    else impl_->model->NullContextWrite(state.storage.data());
    if (start < text.size()) {
        State output; scoreText(state, std::string_view(text).substr(start), output); state = std::move(output);
    }
    return state;
}
}

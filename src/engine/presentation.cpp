// Adapted from phono-fcitx5-addon; Apache-2.0. See data/licenses/PHONO-ADDON-APACHE-2.0.txt.
#include "presentation.h"
#include <nlohmann/json.hpp>
#include <algorithm>

namespace euphony {
std::vector<InvalidInput> invalidFromSegmentation(const nlohmann::json &document) {
    const auto input = document.at("input").get<std::string>();
    const auto segments = document.at("segments").get<std::vector<std::string>>();
    std::vector<bool> removed(input.size(), false);
    for (const auto &event : document.at("normalization_events")) {
        if (event.at("type") != "forced_boundary") continue;
        for (size_t i = event.at("begin"); i < event.at("end") && i < input.size(); ++i)
            removed[i] = true;
    }
    std::vector<InvalidInput> result;
    for (const auto &range : document.at("invalid_ranges")) {
        const size_t begin = range.at("begin"), end = range.at("end");
        result.push_back({begin, end, 0, range.at("text")});
        for (size_t i = begin; i < end && i < input.size(); ++i) removed[i] = true;
    }
    // The core normalizes ASCII case and v in-place, removes separators, and
    // returns all other invalid source ranges. Valid token byte lengths thus
    // describe the remaining source bytes without guessing syllable boundaries.
    for (auto &range : result) {
        size_t bytes = 0;
        for (size_t i = 0; i < range.begin && i < input.size(); ++i)
            if (!removed[i]) ++bytes;
        size_t consumed = 0;
        while (range.beforeSyllable < segments.size() &&
               consumed + segments[range.beforeSyllable].size() <= bytes) {
            consumed += segments[range.beforeSyllable].size();
            ++range.beforeSyllable;
        }
    }
    return result;
}
CandidatePresentation presentCandidate(const std::string &decoded,
    const std::vector<InvalidInput> &invalid, InvalidInputPolicy policy) {
    std::vector<std::string> characters;
    for (size_t i = 0; i < decoded.size();) {
        const auto lead = static_cast<unsigned char>(decoded[i]);
        const size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        characters.push_back(decoded.substr(i, length));
        i += length;
    }
    CandidatePresentation result;
    auto append = [&](const std::string &text, bool deleted) {
        if (text.empty()) return;
        if (!result.pieces.empty() && result.pieces.back().deleted == deleted)
            result.pieces.back().text += text;
        else result.pieces.push_back({text, deleted});
        if (!deleted) result.committed += text;
    };
    // Keep source order even when multiple invalid spans occupy one boundary.
    auto ranges = invalid;
    std::stable_sort(ranges.begin(), ranges.end(), [](const auto &a, const auto &b) {
        return a.begin < b.begin;
    });
    size_t range = 0;
    for (size_t i = 0; i <= characters.size(); ++i) {
        while (range < ranges.size() &&
               std::min(ranges[range].beforeSyllable, characters.size()) == i) {
            append(ranges[range].text, policy == InvalidInputPolicy::Delete);
            ++range;
        }
        if (i < characters.size()) append(characters[i], false);
    }
    return result;
}
} // namespace euphony

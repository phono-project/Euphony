// Adapted from phono-fcitx5-addon; Apache-2.0. See data/licenses/PHONO-ADDON-APACHE-2.0.txt.
#pragma once
#include "worker.h"
#include <nlohmann/json_fwd.hpp>

namespace euphony {
struct CandidatePiece {
    std::string text;
    bool deleted = false;
    bool operator==(const CandidatePiece&) const = default;
};
struct CandidatePresentation {
    std::vector<CandidatePiece> pieces;
    std::string committed;
};
// Map the core's source-byte diagnostics to generated syllable positions.
std::vector<InvalidInput> invalidFromSegmentation(const nlohmann::json &document);
CandidatePresentation presentCandidate(const std::string &decoded,
    const std::vector<InvalidInput> &, InvalidInputPolicy);
} // namespace euphony

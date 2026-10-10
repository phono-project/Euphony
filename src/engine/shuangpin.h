#pragma once
#include "worker.h"
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace euphony {
struct ShuangpinConversion {
    std::vector<ExplicitSyllable> tokens;
    std::vector<InvalidInput> invalid;
};
class ShuangpinProfile {
public:
    ShuangpinProfile(std::string_view scheme, const std::unordered_set<std::string>& validSyllables);
    ShuangpinConversion convert(std::string_view input) const;
    bool usesSemicolon() const;
private:
    std::unordered_map<std::string, std::vector<std::string>> table_;
};
void preferShuangpinReading(ShuangpinConversion&, std::string_view fullPinyin);
}

// Fixed-pair conversion semantics adapted from phono-fcitx5-addon, Apache-2.0.
// Profile tables are the limited libime snippets in shuangpin_tables.h (LGPL).
#include "shuangpin.h"
#include "shuangpin_tables.h"
#include "dictionary.h"
#include <algorithm>
#include <stdexcept>
#include <map>

namespace euphony {
ShuangpinProfile::ShuangpinProfile(std::string_view scheme, const std::unordered_set<std::string>& valid) {
    const SP_C* finals = nullptr;
    const SP_S* initials = nullptr;
    std::string zero = "o";
    if (scheme == "xiaohe") { finals = SPMap_C_XIAOHE; initials = SPMap_S_XIAOHE; zero = "*"; }
    else if (scheme == "ziranma") { finals = SPMap_C_Ziranma; initials = SPMap_S_Ziranma; zero = "o*"; }
    else if (scheme == "ms") { finals = SPMap_C_MS; initials = SPMap_S_MS; }
    else if (scheme == "ziguang") { finals = SPMap_C_Ziguang; initials = SPMap_S_Ziguang; }
    else if (scheme == "abc") { finals = SPMap_C_ABC; initials = SPMap_S_ABC; }
    else if (scheme == "zhongwenzhixing") { finals = SPMap_C_Zhongwenzhixing; initials = SPMap_S_Zhongwenzhixing; }
    else if (scheme == "pinyinjiajia") { finals = SPMap_C_PinyinJiaJia; initials = SPMap_S_PinyinJiaJia; zero = "o*"; }
    else throw std::invalid_argument("Unknown shuangpin profile");
    std::map<char, std::vector<std::string>> finalMap, initialMap;
    std::map<std::string, char> singleFinal;
    for (const auto* p = finals; p->cJP; ++p) {
        finalMap[p->cJP].emplace_back(p->strQP);
        if (std::string_view(p->strQP).size() == 1) singleFinal[p->strQP] = p->cJP;
    }
    for (char c : std::string("aeiouv"))
        if (!singleFinal.contains(std::string(1, c))) {
            finalMap[c].emplace_back(1, c); singleFinal[std::string(1, c)] = c;
        }
    for (const auto* p = initials; p->cJP; ++p) initialMap[p->cJP].emplace_back(p->strQP);
    for (char c : std::string("bpmfdtnlgkhjqxrzcsyw")) initialMap[c].emplace_back(1, c);
    for (char c : zero) if (c != '*') initialMap[c].emplace_back();
    auto add = [&](std::string pair, std::string spelling) {
        // The dictionary exports canonical spellings; j/q/x/y + v are
        // normalized to u as at the core boundary.
        if (spelling.size() > 1 && std::string("jqxy").find(spelling[0]) != std::string::npos)
            for (auto& c : spelling) if (c == 'v') c = 'u';
        if (!valid.contains(spelling)) return;
        auto& alternatives = table_[pair];
        if (std::find(alternatives.begin(), alternatives.end(), spelling) == alternatives.end()) alternatives.push_back(spelling);
    };
    for (const auto& [first, prefixes] : initialMap)
        for (const auto& [last, suffixes] : finalMap)
            for (const auto& prefix : prefixes)
                for (const auto& suffix : suffixes) add(std::string{first,last}, prefix + suffix);
    if (zero.find('*') != std::string::npos)
        for (const auto& [key, suffixes] : finalMap)
            for (const auto& suffix : suffixes) {
                if (!valid.contains(suffix)) continue;
                if (suffix.size() == 1) add(std::string{key,key}, suffix);
                else if (singleFinal.contains(suffix.substr(0,1))) add(std::string{singleFinal[suffix.substr(0,1)],key}, suffix);
            }
    for (const auto& syllable : valid)
        if (syllable.size() == 2 && syllable != "ng" && std::string("aeo").find(syllable[0]) != std::string::npos &&
            (!table_.contains(syllable) || zero.find('*') != std::string::npos)) add(syllable,syllable);
    // libime orders alternatives by initial/final enum, placing o before uo.
    // All other ambiguous finals use canonical lexical order.
    for (auto& [pair, alternatives] : table_) { (void)pair; std::sort(alternatives.begin(),alternatives.end()); }
}
bool ShuangpinProfile::usesSemicolon() const {
    return std::any_of(table_.begin(), table_.end(), [](const auto& pair) { return pair.first.find(';') != std::string::npos; });
}
ShuangpinConversion ShuangpinProfile::convert(std::string_view input) const {
    ShuangpinConversion out;
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 'a' - 'A') : c; };
    auto key = [](char c) { return (c >= 'a' && c <= 'z') || c == ';'; };
    auto invalid = [&](size_t begin,size_t end) {
        if (!out.invalid.empty() && out.invalid.back().end == begin) {
            out.invalid.back().end = end; out.invalid.back().text += input.substr(begin,end-begin);
        } else out.invalid.push_back({begin,end,out.tokens.size(),std::string(input.substr(begin,end-begin))});
    };
    auto split = [&](size_t i) {
        char c = lower(input[i]);
        if (std::string_view("abcdefghjklmnopqrstwxyz").find(c) != std::string_view::npos)
            out.tokens.push_back({i,i+1,{std::string(1,c)}});
        else invalid(i,i+1);
    };
    for (size_t i=0;i<input.size();) {
        char c=lower(input[i]);
        if (c=='\'' || c==' ' || c=='\t' || c=='\r' || c=='\n') { ++i; continue; }
        if (!key(c)) { invalid(i,i+1); ++i; continue; }
        if (i+1<input.size() && key(lower(input[i+1]))) {
            auto it=table_.find(std::string{c,lower(input[i+1])});
            if (it!=table_.end()) out.tokens.push_back({i,i+2,it->second});
            else { split(i); split(i+1); }
            i+=2;
        } else split(i++);
    }
    return out;
}
void preferShuangpinReading(ShuangpinConversion& converted,std::string_view pinyin) {
    if (!converted.invalid.empty()) return;
    auto readings=splitPinyin(pinyin);
    if (readings.size()!=converted.tokens.size()) return;
    for (size_t i=0;i<readings.size();++i)
        if (std::find(converted.tokens[i].alternatives.begin(),converted.tokens[i].alternatives.end(),readings[i])==converted.tokens[i].alternatives.end()) return;
    for (size_t i=0;i<readings.size();++i) {
        auto& alternatives=converted.tokens[i].alternatives;
        auto preferred=std::find(alternatives.begin(),alternatives.end(),readings[i]);
        std::rotate(alternatives.begin(),preferred,preferred+1);
    }
}
}

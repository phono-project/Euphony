#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace euphony::tsf {
inline bool deferred_key(std::string_view key) {
    return key.size()==1 || key=="Backspace" || key=="Delete" || key=="Left" || key=="Right" ||
        key=="Home" || key=="End" || key=="Up" || key=="Down" || key=="Space" || key=="Enter" ||
        key=="Escape" || key=="Tab" || key=="PageUp" || key=="PageDown";
}
// TSF must decide whether to consume a key before an IPC reply arrives.
// Keep this deliberately local and deterministic; model work stays in the broker.
inline bool consumes(std::string_view key, bool active, bool shift, bool ctrl, bool alt, bool full_width=false, bool chinese_punctuation=true, bool full_width_digits=false, bool full_width_symbols=false) {
    if (ctrl || alt) return false;
    const bool digit=key.size()==1 && key[0]>='0' && key[0]<='9';
    const bool symbol=key.size()==1 && std::string_view("~@#%&*-=+|/").find(key[0])!=std::string_view::npos;
    if (full_width && (!digit || full_width_digits) && (!symbol || full_width_symbols) &&
        (key=="Space" || (key.size()==1 && key[0]>=32 && key[0]<127))) return true;
    if (key == "Tab") return active;
    if (shift && key.size() != 1) return false;
    if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z' && !shift) return true;
    if (!active) return chinese_punctuation && key.size() == 1 && std::string_view(",.!?:;\"'()[]<>-+/\\$^_|{}").find(key[0]) != std::string_view::npos;
    return key.size() == 1 || key == "Backspace" || key == "Delete" || key == "Left" || key == "Right" ||
        key == "Home" || key == "End" || key == "Up" || key == "Down" || key == "Space" ||
        key == "Enter" || key == "Escape" || key == "PageUp" || key == "PageDown";
}
inline std::size_t utf16_boundary(std::wstring_view text, std::size_t cursor) {
    cursor = cursor > text.size() ? text.size() : cursor;
    if (cursor && cursor < text.size() && text[cursor] >= 0xdc00 && text[cursor] <= 0xdfff &&
        text[cursor-1] >= 0xd800 && text[cursor-1] <= 0xdbff) --cursor;
    return cursor;
}
inline bool accepts_response(std::string_view expected, std::string_view received,
                             std::uint64_t last_seq, std::uint64_t seq,
                             std::uint64_t generation, std::uint64_t received_generation) {
    return expected == received && seq >= last_seq && received_generation >= generation;
}
}

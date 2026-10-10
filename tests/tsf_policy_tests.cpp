#include "tsf/key_policy.h"
#include <iostream>
#include <stdexcept>

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    using namespace euphony::tsf;
    try {
        require(consumes("a",false,false,false,false),"Pinyin starts without a broker round trip");
        require(!consumes("a",false,false,true,false),"Application shortcuts must pass through");
        require(consumes("Tab",true,true,false,false),"Reverse candidate paging preserves Shift");
        require(!consumes("Backspace",false,false,false,false),"Idle Backspace belongs to application");
        require(consumes("Backspace",true,false,false,false),"Active Backspace belongs to composition");
        require(!accepts_response("next","old",4,6,3,5),"A late reply must not enter a different document session");
        require(!accepts_response("same","same",6,5,3,4),"A late sequence must not replay a commit");
        require(!accepts_response("same","same",6,6,5,4),"A late model generation must not overwrite navigation");
        require(accepts_response("same","same",6,7,5,5),"Current generation poll replies must be accepted");
        const std::wstring emoji=L"a\xd83d\xde00\x4e2d";
        require(utf16_boundary(emoji,2)==1,"Composition cursor cannot split a surrogate pair");
        require(utf16_boundary(emoji,3)==3,"Cursor after supplementary character must remain stable");
        require(utf16_boundary(emoji,99)==4,"Out of range cursor is bounded");
        std::cout<<"TSF key ownership, stale response and UTF-16 checks passed\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}

#pragma once
#include <cstdint>
namespace arvr {
// Bit order: up, down, left, right, confirm, back, pause.
struct MenuKeyEvents {uint32_t down{},up{},repeat{};};
class MenuKeys {
    uint32_t held=0;uint64_t due[7]{};
public:
    MenuKeyEvents Update(uint32_t mask,uint64_t now){
        MenuKeyEvents e{mask&~held,held&~mask,0};
        for(unsigned i=0;i<7;++i){const uint32_t bit=1u<<i;
            if(e.down&bit)due[i]=now+400;
            else if(i<4 && (mask&bit) && now>=due[i]){e.repeat|=bit;due[i]=now+140;}
        }
        held=mask;return e;
    }
};
}

#pragma once
#include <cstdint>
namespace arvr {
inline bool FrameFresh(uint64_t captured, uint64_t now, uint64_t maxAge=250){
    return captured && now>=captured && now-captured<maxAge;
}
}

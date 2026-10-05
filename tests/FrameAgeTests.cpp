#include "shared/FrameAge.h"
#include <cstdio>
int main(){
    using arvr::FrameFresh;
    if(FrameFresh(0,100)||FrameFresh(101,100)||!FrameFresh(100,100)||!FrameFresh(100,349)||FrameFresh(100,350))return 1;
    // Resubmitting a cached frame must never renew its source timestamp.
    for(uint64_t now=100;now<1000;++now)if(FrameFresh(100,now)!=(now<350))return 2;
    puts("PASS: frame expiry, future timestamp rejection, repeated cache submission cannot extend lifetime");
}

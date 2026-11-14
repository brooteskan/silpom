// SPDX-License-Identifier: MIT
#include "CpuShader.h"
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
using namespace SilPOM::Cpu;
int main()
{
    std::mt19937 rng(61837);
    const int sizes[]={1,2,3,7,511,512,513,6144,6656,8192,16384,65535};
    const int extremes[]={std::numeric_limits<int>::min(),std::numeric_limits<int>::max(),-1,0,1};
    unsigned count=0;
    for(int size:sizes)for(unsigned query=0;query<200000;++query)
    {
        const int i=query<5?extremes[query]:static_cast<int>(rng());
        // Independent 64-bit floor-division reference; includes negative UV
        // repeats, INT_MIN, clamp, and the actual non-power-of-two atlas sizes.
        const std::int64_t dividend=i,divisor=size;
        std::int64_t quotient=dividend/divisor;
        if(dividend<0&&dividend%divisor) --quotient;
        const auto expected=static_cast<int>(dividend-quotient*divisor);
        if(SilPomAddress(i,size,0)!=expected||SilPomAddress(i,size,1)!=std::min(std::max(i,0),size-1))
        {std::cerr<<"Address mismatch: "<<i<<" / "<<size<<'\n';return 1;}
        ++count;
    }
    std::cout<<count<<" repeat/clamp addresses match the floor-division reference exactly\n";
}

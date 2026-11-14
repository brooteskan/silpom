// SPDX-License-Identifier: MIT
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
namespace SilPOM::MaterialCodec
{
inline constexpr uint32_t Inputs=12,Hidden=16,Outputs=6,Weights=310,Version=1;
struct Header
{
    char magic[8];
    uint32_t version,width,height,mips,sourceWidth,sourceHeight,weights;
    uint8_t sourceHash[32];
};
static_assert(sizeof(Header)==68);
inline size_t PixelCount(uint32_t w,uint32_t h,uint32_t mips)
{
    size_t n=0;for(uint32_t i=0;i<mips;++i){n+=size_t(w)*h;w=w>1?w/2:1;h=h>1?h/2:1;}return n;
}
inline bool Validate(const Header& h,size_t bytes,const float* weights)
{
    if(std::memcmp(h.magic,"SILPMAT4",8)||h.version!=Version||!h.width||!h.height
        ||h.width!=1664||h.height!=1536||h.sourceWidth!=6656||h.sourceHeight!=6144||h.weights!=Weights) return false;
    uint32_t levels=1,w=h.width,v=h.height;
    while(w>1||v>1){w=w>1?w/2:1;v=v>1?v/2:1;++levels;}
    if(h.mips!=levels||bytes!=sizeof(Header)+Weights*4+PixelCount(h.width,h.height,h.mips)*8)return false;
    for(uint32_t i=0;i<Weights;++i)if(!std::isfinite(weights[i]))return false;
    return true;
}
inline void Decode(const float* x,const float* w,float* out)
{
    for(uint32_t c=0;c<Outputs;++c)out[c]=w[304+c];
    for(uint32_t j=0;j<Hidden;++j)
    {
        float z=w[192+j];for(uint32_t k=0;k<Inputs;++k)z+=x[k]*w[j*Inputs+k];
        z=z>0?z:0;for(uint32_t c=0;c<Outputs;++c)out[c]+=z*w[208+j*Outputs+c];
    }
    for(uint32_t c=0;c<Outputs;++c)out[c]=out[c]<0?0:out[c]>1?1:out[c];
}
}

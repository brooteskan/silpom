// SPDX-License-Identifier: MIT
#pragma once
#include "MaterialCodec.h"
namespace SilPOM::AppearanceCodec
{
inline constexpr uint32_t SurfaceInputs=18,LightInputs=22,Hidden=16,SurfaceOutputs=3,LightOutputs=2;
inline constexpr uint32_t SurfaceWeights=355,LightWeights=402,Weights=757;
using Header=MaterialCodec::Header;
inline bool Validate(const Header& h,size_t bytes,const float* weights)
{
    if(std::memcmp(h.magic,"SILPAPP5",8)||h.version!=2||h.width!=832||h.height!=768||h.mips!=10
        ||h.sourceWidth!=6656||h.sourceHeight!=6144||h.weights!=Weights)return false;
    if(bytes!=sizeof(Header)+Weights*4+MaterialCodec::PixelCount(h.width,h.height,h.mips)*8)return false;
    for(uint32_t i=0;i<Weights;++i)if(!std::isfinite(weights[i]))return false;
    return true;
}
inline float Logistic(float x){return 1.f/(1.f+std::exp(-std::fmax(-16.f,std::fmin(16.f,x))));}
inline void Decode(const float* x,const float* w,uint32_t inputs,uint32_t outputs,float* result)
{
    const uint32_t bias=inputs*Hidden,second=bias+Hidden,last=second+Hidden*outputs;
    for(uint32_t c=0;c<outputs;++c)result[c]=w[last+c];
    for(uint32_t j=0;j<Hidden;++j)
    {
        float z=w[bias+j];for(uint32_t k=0;k<inputs;++k)z+=x[k]*w[j*inputs+k];
        z=std::fmax(z,0.f);for(uint32_t c=0;c<outputs;++c)result[c]+=z*w[second+j*outputs+c];
    }
    for(uint32_t c=0;c<outputs;++c)result[c]=Logistic(result[c]);
}
}

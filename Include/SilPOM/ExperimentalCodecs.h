// SPDX-License-Identifier: MIT
#pragma once
#include "AppearanceCodec.h"
namespace SilPOM::SharedCodec
{
inline constexpr uint32_t Inputs=20,Hidden=12,Weights=575,Channels=12;
inline bool Validate(const MaterialCodec::Header& h,size_t bytes,const float* weights)
{
    if(std::memcmp(h.magic,"SILPSHR4",8)||h.version!=2||h.width!=1664||h.height!=1536||h.mips!=11
        ||h.sourceWidth!=6656||h.sourceHeight!=6144||h.weights!=Weights)return false;
    if(bytes!=sizeof(h)+Weights*4+MaterialCodec::PixelCount(h.width,h.height,h.mips)*Channels)return false;
    for(uint32_t i=0;i<Weights;++i)if(!std::isfinite(weights[i]))return false;
    return true;
}
inline float Decode(const float* x,const float* w)
{
    float result=w[574];
    for(uint32_t j=0;j<Hidden;++j){float z=w[550+j];for(uint32_t k=0;k<Inputs;++k)z+=x[k]*w[310+j*Inputs+k];
        result+=std::fmax(z,0.f)*w[562+j];}
    return AppearanceCodec::Logistic(result);
}
}
namespace SilPOM::DepthCodec
{
inline constexpr uint32_t Inputs=23,Hidden=16,HeadWeights=401,Weights=AppearanceCodec::Weights+HeadWeights;
inline bool Validate(const MaterialCodec::Header& h,size_t bytes,const float* weights)
{
    if(std::memcmp(h.magic,"SILPDEP5",8)||h.version!=2||h.width!=832||h.height!=768||h.mips!=10
        ||h.sourceWidth!=6656||h.sourceHeight!=6144||h.weights!=Weights)return false;
    if(bytes!=sizeof(h)+Weights*4+MaterialCodec::PixelCount(h.width,h.height,h.mips)*8)return false;
    for(uint32_t i=0;i<Weights;++i)if(!std::isfinite(weights[i]))return false;
    return true;
}
inline float Decode(const float* x,const float* w)
{
    float result;AppearanceCodec::Decode(x,w+AppearanceCodec::Weights,Inputs,1,&result);return result;
}
// Local constant-direction proxy plane. This is a single analytic solve,
// bounded by the supplied displacement interval, with no height validation.
inline float RayOffset(float heightFraction,float scale,float reference,float normalDirection,float normalView)
{
    return scale*(std::fmax(0.f,std::fmin(1.f,heightFraction))-reference)*normalDirection/normalView;
}
}

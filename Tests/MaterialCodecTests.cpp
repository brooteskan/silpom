// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/MaterialCodec.h"
#include <iostream>
#include <limits>
int main()
{
    using namespace SilPOM::MaterialCodec;
    Header h{};memcpy(h.magic,"SILPMAT4",8);h.version=1;h.width=1664;h.height=1536;h.mips=11;
    h.sourceWidth=6656;h.sourceHeight=6144;h.weights=310;
    float w[310]{};size_t bytes=sizeof(h)+1240+PixelCount(h.width,h.height,h.mips)*8;
    if(!Validate(h,bytes,w)||Validate(h,bytes-1,w))return 1;
    auto wrong=h;wrong.width=1024;if(Validate(wrong,bytes,w))return 1;
    wrong=h;wrong.mips=10;if(Validate(wrong,bytes,w))return 1;
    w[20]=std::numeric_limits<float>::infinity();if(Validate(h,bytes,w))return 1;w[20]=0;
    float x[12]{},y[6]{};for(int i=0;i<6;++i){w[i*12+i]=1;w[208+i*6+i]=1;w[304+i]=.25f;x[i]=.5f;}
    Decode(x,w,y);for(float v:y)if(v!=.75f)return 1;
    for(int i=0;i<6;++i)x[i]=-1;Decode(x,w,y);for(float v:y)if(v!=.25f)return 1;
    for(int i=0;i<6;++i)x[i]=10;Decode(x,w,y);for(float v:y)if(v!=1)return 1;
    std::cout<<"Material bundle validation, finite coefficients, channel layout and bounded decoder passed\n";
}

// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/ReliefLod.h"
#include "../Include/SilPOM/AppearanceCodec.h"
#include <iostream>
#include <random>
using namespace SilPOM;
int main()
{
    if(ReliefLod::Select(false,false,.5,.8,1)||!ReliefLod::Select(true,false,.5,.8,1)
        ||!ReliefLod::Select(false,false,.5,.7,1)||ReliefLod::Select(true,false,.5,1.01,1)
        ||ReliefLod::Select(true,true,1,.3,1)||!ReliefLod::Select(false,true,1,.1,1)
        ||ReliefLod::Select(true,true,.49,0,1)||ReliefLod::Select(true,false,.349,0,1))return 1;
    std::mt19937 rng(5713);std::uniform_real_distribution<double> distribution(0,1);
    auto random=[&](){return distribution(rng);};
    for(int trial=0;trial<100;++trial)
    {
        std::array<ReliefLod::Clip,3> base{{{-.3,-.2,.3,2},{.5,-.1,.2,2.2},{-.2,.7,.3,3}}};
        std::array<ReliefLod::Clip,6> relief;
        for(size_t i=0;i<6;++i)
        {
            auto b=base[i/2];const double h=i%2?.08:-.02;
            relief[i]={b.x+h*(random()-.5),b.y+h*(random()-.5),b.z,b.w+h*(random()-.5)};
        }
        const double bound=ReliefLod::PixelError(base,relief,1600,900);
        if(std::abs(ReliefLod::PixelError(base,relief,3200,1800)-2*bound)>1e-9)return 1;
        for(int sample=0;sample<100;++sample)
        {
            double a=random(),b=random();if(a+b>1){a=1-a;b=1-b;}const double weights[]={a,b,1-a-b};
            const double h=random();ReliefLod::Clip p{},q{};
            for(size_t i=0;i<3;++i)
            {
                const auto& lo=relief[2*i];const auto& hi=relief[2*i+1];
                p.x+=weights[i]*base[i].x;p.y+=weights[i]*base[i].y;p.w+=weights[i]*base[i].w;
                q.x+=weights[i]*(lo.x*(1-h)+hi.x*h);q.y+=weights[i]*(lo.y*(1-h)+hi.y*h);q.w+=weights[i]*(lo.w*(1-h)+hi.w*h);
            }
            if(std::hypot((p.x/p.w-q.x/q.w)*800,(p.y/p.w-q.y/q.w)*450)>bound+1e-9)return 1;
        }
        relief[0].w=0;if(std::isfinite(ReliefLod::PixelError(base,relief,1600,900)))return 1;
    }
    AppearanceCodec::Header h{};memcpy(h.magic,"SILPAPP5",8);h.version=2;h.width=832;h.height=768;h.mips=10;
    h.sourceWidth=6656;h.sourceHeight=6144;h.weights=AppearanceCodec::Weights;
    const size_t bytes=sizeof(h)+h.weights*4+MaterialCodec::PixelCount(h.width,h.height,h.mips)*8;
    float w[AppearanceCodec::Weights]{};if(!AppearanceCodec::Validate(h,bytes,w)||AppearanceCodec::Validate(h,bytes-1,w))return 1;
    auto wrong=h;memcpy(wrong.magic,"SILPMAT4",8);if(AppearanceCodec::Validate(wrong,bytes,w))return 1;
    wrong=h;wrong.version=1;if(AppearanceCodec::Validate(wrong,bytes,w))return 1;
    float x[22]{},s[3],l[2];AppearanceCodec::Decode(x,w,18,3,s);AppearanceCodec::Decode(x,w+355,22,2,l);
    for(float v:s)if(v!=.5f)return 1;for(float v:l)if(v!=.5f)return 1;
    w[0]=std::numeric_limits<float>::quiet_NaN();if(AppearanceCodec::Validate(h,bytes,w))return 1;
    std::cout<<"Projective error bound, viewport scaling, hysteresis, boundary/grazing guards and appearance bundle passed\n";
}

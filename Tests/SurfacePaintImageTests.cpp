// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/SurfacePaintImage.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace SilPOM::SurfacePaintMath;
namespace Images=SilPOM::SurfacePaintImage;

void Require(bool pass,const char* message)
{
    if(!pass){std::fprintf(stderr,"%s\n",message);std::exit(1);}
}
bool Near(float a,float b){return std::abs(a-b)<2.e-6f;}
bool Near(Vec3 a,Vec3 b){return Near(a.x,b.x)&&Near(a.y,b.y)&&Near(a.z,b.z);}
Images::Image Blank(uint32_t width=32,uint32_t height=32)
{
    Images::Image image;image.width=width;image.height=height;image.pixels.resize(size_t(width)*height);return image;
}
bool SameBits(const Images::Image& a,const Images::Image& b)
{
    return a.width==b.width&&a.height==b.height&&a.pixels.size()==b.pixels.size()
        &&std::memcmp(a.pixels.data(),b.pixels.data(),a.pixels.size()*sizeof(Images::Pixel))==0;
}
int main()
{
    auto image=Blank();
    Require(Images::Valid(image),"Empty chart is valid source paint");
    const auto blank=Images::Encode(image);
    Require(!blank.empty()&&blank.size()<64,"Transparent chart uses compact lossless runs");
    Images::Image decoded;
    Require(Images::Decode(blank,decoded)&&SameBits(image,decoded),"Transparent chart survives persistence");

    // The transient brush sequence can go out of scope. Its resolved texels
    // alone retain both painted channels, including coverage reduced by erase.
    std::string flattened;
    Images::Pixel expected;
    {
        auto work=Blank();auto& texel=work.pixels[7];
        PaintColor(texel.color,{1,0,0},.5f);PaintColor(texel.color,{0,0,1},.5f);
        PaintNormal(texel.normal,{.8f,0,.6f},.75f);PaintNormal(texel.normal,{0,.6f,.8f},.25f);
        EraseColor(texel.color,.5f);EraseNormal(texel.normal,.5f);
        texel.color.premultiplied.y=-0.f;expected=texel;
        flattened=Images::Encode(work);
    }
    Require(Images::Decode(flattened,decoded),"Flattened paint restores without any source dab list");
    Require(std::memcmp(&decoded.pixels[7],&expected,sizeof(expected))==0,"Storage preserves exact float bits, including negative zero");
    Require(Near(expected.color.premultiplied,{.125f,0,.25f})&&Near(expected.color.coverage,.375f),"Erase reveals base instead of resurrecting overwritten paint");
    Require(Near(ComposeColor({.2f,.4f,.6f},decoded.pixels[7].color),{.25f,.25f,.625f}),"Restored coverage composes over the live base material");
    Require(Near(ComposeNormal(Unit({.3f,-.2f,1}),expected.normal),ComposeNormal(Unit({.3f,-.2f,1}),decoded.pixels[7].normal)),"Normal detail has no dependency on a saved material normal");

    auto original=decoded;
    auto rejected=[&](std::string_view text,const char* message)
    {
        Require(!Images::Decode(text,decoded),message);
        Require(SameBits(original,decoded),"Failed decode never replaces valid source paint");
    };
    rejected({},"Empty payload rejected");
    rejected(std::string_view(flattened).substr(1),"Truncated Base64 rejected");
    rejected(std::string_view(flattened).substr(0,flattened.size()-4),"Truncated binary stream rejected");
    auto changed=flattened;changed[0]='!';rejected(changed,"Invalid Base64 alphabet rejected");
    changed=flattened;changed[1]='=';rejected(changed,"Invalid Base64 padding rejected");
    std::vector<uint8_t> bytes;
    Require(Images::Detail::Unbase64(blank,bytes),"Test fixture decodes to binary");
    auto badBytes=bytes;badBytes[0]^=1;rejected(Images::Detail::Base64(badBytes),"Unknown payload version rejected");
    badBytes=bytes;badBytes[16]^=1;rejected(Images::Detail::Base64(badBytes),"Changed checksum rejected");
    badBytes=bytes;badBytes[4]=33;rejected(Images::Detail::Base64(badBytes),"Non-power-of-two dimensions rejected");
    badBytes=bytes;badBytes[5]=8;rejected(Images::Detail::Base64(badBytes),"Dimensions beyond allocation bound rejected");
    badBytes=bytes;badBytes[20]=badBytes[21]=badBytes[22]=0;rejected(Images::Detail::Base64(badBytes),"Zero-length run rejected");
    badBytes=bytes;badBytes[20]+=1;rejected(Images::Detail::Base64(badBytes),"Run past declared pixel payload rejected");
    badBytes=bytes;badBytes[23]&=0x7f;rejected(Images::Detail::Base64(badBytes),"Literal run without enough bytes rejected");
    badBytes=bytes;badBytes.push_back(0);rejected(Images::Detail::Base64(badBytes),"Trailing bytes rejected");
    badBytes.clear();
    constexpr uint32_t words=32*32*8,nanBits=0x7fc00000;
    uint32_t hash=2166136261u;
    for(uint32_t i=0;i<words;++i)hash=Images::Detail::HashWord(hash,nanBits);
    for(uint32_t word:{0x31545053u,32u,32u,words,hash,0x80000000u|words,nanBits})Images::Detail::AddWord(badBytes,word);
    rejected(Images::Detail::Base64(badBytes),"Nonfinite texels rejected even with a matching checksum");
    auto invalid=Blank();invalid.pixels[0].normal.premultiplied={1,0,0};
    Require(Images::Encode(invalid).empty(),"Normal vector cannot exceed its coverage");
    invalid=Blank();invalid.pixels[0].color.coverage=1.1f;
    Require(Images::Encode(invalid).empty(),"Invalid coverage cannot be persisted");
    invalid=Blank();invalid.pixels[0].color.premultiplied={.5f,0,0};
    Require(Images::Encode(invalid).empty(),"Premultiplied color cannot exceed its coverage");
    invalid=Blank();invalid.pixels[0].color.premultiplied.x=std::numeric_limits<float>::infinity();
    Require(Images::Encode(invalid).empty(),"Nonfinite color cannot be persisted");
    invalid=Blank();invalid.pixels.pop_back();
    Require(Images::Encode(invalid).empty(),"Mismatched payload size cannot be persisted");

    // Bilinear sampling follows GPU texel centers and clamped chart gutters.
    auto sample=Blank();PaintColor(sample.pixels[0].color,{1,0,0},1);PaintColor(sample.pixels[1].color,{0,0,1},1);
    Require(Near(Images::Sample(sample,.5f/32,.5f/32).color.premultiplied,{1,0,0}),"First texel center samples exactly");
    Require(Near(Images::Sample(sample,1.f/32,.5f/32).color.premultiplied,{.5f,0,.5f}),"Linear sampling averages premultiplied values");
    Require(Near(Images::Sample(sample,-1,-1).color.premultiplied,{1,0,0}),"Chart edge samples clamp");
    Require(Images::Sample(sample,std::numeric_limits<float>::quiet_NaN(),0).color.coverage==0,"Nonfinite coordinates do not address memory");

    auto neutral=Blank();
    for(auto& pixel:neutral.pixels){PaintColor(pixel.color,{.1f,.2f,.3f},.5f);PaintNormal(pixel.normal,{0,0,1},.5f);}
    const auto mips=Images::GenerateMips(neutral);
    Require(mips.size()==6&&mips.front().width==32&&mips.back().width==1,"Complete mip chain reaches one texel");
    const auto base=Unit({.3f,-.4f,1});
    for(const auto& mip:mips)
    {
        Require(Images::Valid(mip),"Every mip remains a valid premultiplied image");
        for(const auto& pixel:mip.pixels)
        {
            Require(Near(pixel.normal.coverage,.5f)&&Near(pixel.normal.premultiplied,{0,0,.5f}),"Mips retain normal coverage without normalization");
            Require(Near(ComposeNormal(base,pixel.normal),base),"Flat normal detail remains neutral at every mip");
        }
    }
    auto opposed=Blank();
    for(size_t i=0;i<opposed.pixels.size();++i)PaintNormal(opposed.pixels[i].normal,{i%2?.8f:-.8f,0,.6f},1);
    const auto opposedMips=Images::GenerateMips(opposed);
    Require(Near(opposedMips.back().pixels[0].normal.premultiplied,{0,0,.6f}),"Normal vector magnitude survives directional cancellation in mips");
    auto rect=Blank(64,32);
    Require(Images::GenerateMips(rect).size()==7,"Rectangular power-of-two charts also reach one texel");
    std::puts("Surface paint persistence, validation, lossless flattening, bilinear sampling and mip filtering passed");
}

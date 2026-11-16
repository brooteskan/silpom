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
    Require(Near(ComposeColor({.2f,.4f,.6f},decoded.pixels[7].color),{.21348120f,.26213777f,.59503084f}),"Restored linear-sRGB paint converts before composing over the live ACEScg material");
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

    const Images::Triangle triangle{{{0,0},{1,0},{0,1}}};
    auto face=Blank(256,256);
    for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x)
        if(x+y<=259) // Reproduce the rasterizer's two-texel barycentric margin.
        {auto& p=face.pixels[size_t(y)*256+x];PaintColor(p.color,{1,.2f,.1f},1);PaintNormal(p.normal,{.8f,0,.6f},1);}
    Require(Images::GenerateMips(face).back().pixels[0].color.coverage<.53f,"Old rectangular reduction reproduces triangle transparency dilution");
    const auto savedFace=face;
    auto faceMips=Images::GenerateMips(face,triangle);
    Require(faceMips.size()==9&&SameBits(face,savedFace),"Domain mips are derived without changing persistent source texels");
    for(const auto& mip:faceMips)
    {
        Require(Images::Valid(mip),"Domain-aware mips remain valid premultiplied images");
        for(const auto& p:mip.pixels)
            Require(Near(p.color.coverage,1)&&Near(p.normal.coverage,1)
                &&Near(p.color.premultiplied,{1,.2f,.1f})&&Near(p.normal.premultiplied,{.8f,0,.6f}),
                "A fully painted receiving face stays opaque with unchanged detail through 2x2 and 1x1 mips");
    }

    // Values outside the receiving domain are storage, not surface coverage.
    // They must be replaced as padding, rather than contribute to mip averages.
    face=Blank();unsigned domainCount=0,paintCount=0;
    Vec3 momentSum{};
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)
    {
        auto& p=face.pixels[size_t(y)*32+x];
        if(x+y<=31)
        {
            ++domainCount;
            if(x<8)
            {++paintCount;PaintColor(p.color,{1,0,0},1);PaintNormal(p.normal,{.8f,0,.6f},1);momentSum=momentSum+p.normal.premultiplied;}
        }
        else {PaintColor(p.color,{0,1,0},1);PaintNormal(p.normal,{-.6f,0,.8f},1);}
    }
    const auto partialSource=face;faceMips=Images::GenerateMips(face,triangle);
    Require(!faceMips.empty()&&SameBits(face,partialSource),"Partial paint remains saved exactly");
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)if(x+y<=31)
        Require(std::memcmp(&faceMips[0].pixels[size_t(y)*32+x],&face.pixels[size_t(y)*32+x],sizeof(Images::Pixel))==0,
            "Boundary extension never fills clear or erased receiving texels");
    const float coverage=float(paintCount)/domainCount;const auto& last=faceMips.back().pixels[0];
    Require(Near(last.color.coverage,coverage)&&Near(last.normal.coverage,coverage)
        &&Near(last.color.premultiplied,{coverage,0,0})&&Near(last.normal.premultiplied,momentSum*(1.f/domainCount)),
        "Coarse coverage includes clear receiving area and excludes all outside storage");
    Require(faceMips[0].pixels[31*32+31].color.premultiplied.y==0,"Outside source garbage cannot become domain padding");

    // Erasing a genuine surface texel must survive padding and participate in
    // the area-weighted coarse result, even when surrounded by opaque paint.
    face=Blank();
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)if(x+y<=31)
    {auto& p=face.pixels[size_t(y)*32+x];PaintColor(p.color,{.2f,.4f,.6f},1);PaintNormal(p.normal,{0,.6f,.8f},1);}
    face.pixels[6*32+6]={};faceMips=Images::GenerateMips(face,triangle);
    Require(faceMips.front().pixels[6*32+6].color.coverage==0&&faceMips.front().pixels[6*32+6].normal.coverage==0,
        "Padding preserves a fully erased texel within the triangle");
    Require(Near(faceMips.back().pixels[0].color.coverage,1.f-1.f/domainCount)
        &&Near(faceMips.back().pixels[0].normal.coverage,1.f-1.f/domainCount),"An erased interior texel remains clear coverage in the coarsest mip");

    const Images::Triangle reversed{{{0,1},{1,0},{0,0}}};
    const auto reversedMips=Images::GenerateMips(face,reversed);
    Require(reversedMips.size()==faceMips.size(),"Both triangle orientations have a receiving domain");
    for(size_t i=0;i<faceMips.size();++i)Require(SameBits(faceMips[i],reversedMips[i]),"Domain mip generation is winding-independent");
    const Images::Triangle thin{{{0,0},{1,1},{.5f,.50001f}}};
    auto solid=Blank();for(auto& p:solid.pixels){PaintColor(p.color,{.2f,.3f,.4f},1);PaintNormal(p.normal,{0,0,1},1);}
    const auto thinMips=Images::GenerateMips(solid,thin);
    Require(thinMips.size()==6&&Near(thinMips.back().pixels[0].color.coverage,1)
        &&Near(thinMips.back().pixels[0].normal.coverage,1),"Thin triangles and boundary texel centers do not dilute coarse coverage");
    const Images::Triangle degenerate{{{0,0},{.5f,.5f},{1,1}}};
    Require(Images::GenerateMips(solid,degenerate).empty(),"Degenerate receiving domains do not publish undefined mip data");
    auto badDomain=triangle;badDomain.corners[0].x=std::numeric_limits<float>::infinity();
    Require(Images::GenerateMips(solid,badDomain).empty(),"Nonfinite receiving domains are rejected");
    const Images::Triangle absent{{{2,2},{3,2},{2,3}}};
    Require(Images::GenerateMips(solid,absent).empty(),"A chart without receiving texel centers cannot manufacture surface paint");
    const Images::Triangle whole{{{-2,-2},{4,-2},{-2,4}}};
    Require(Images::GenerateMips(rect,whole).size()==7,"Rectangular charts with one-dimensional coarse mips preserve their domain counts");
    std::puts("Surface paint persistence, validation, lossless flattening, bilinear sampling and mip filtering passed");
}

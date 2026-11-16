// SPDX-License-Identifier: MIT
#pragma once
#include "SurfacePaintMath.h"
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace SilPOM::SurfacePaintImage
{
struct Pixel
{
    SurfacePaintMath::ColorTexel color;
    SurfacePaintMath::NormalTexel normal;
};
static_assert(sizeof(Pixel)==32&&std::is_standard_layout_v<Pixel>&&std::is_trivially_copyable_v<Pixel>);
static_assert(sizeof(float)==4&&std::numeric_limits<float>::is_iec559);
struct Image
{
    uint32_t width=0,height=0;
    std::vector<Pixel> pixels;
};
inline constexpr uint32_t MinResolution=32,MaxResolution=1024;

namespace Detail
{
inline bool Dimension(uint32_t value)
{
    return value&&value<=MaxResolution&&(value&(value-1))==0;
}
inline uint32_t Word(const Image& image,size_t index)
{
    uint32_t word;
    std::memcpy(&word,reinterpret_cast<const unsigned char*>(image.pixels.data())+index*4,4);
    return word;
}
inline void SetWord(Image& image,size_t index,uint32_t word)
{
    std::memcpy(reinterpret_cast<unsigned char*>(image.pixels.data())+index*4,&word,4);
}
inline void AddWord(std::vector<uint8_t>& bytes,uint32_t word)
{
    for(unsigned i=0;i<4;++i)bytes.push_back(uint8_t(word>>(i*8)));
}
inline uint32_t ReadWord(const std::vector<uint8_t>& bytes,size_t offset)
{
    uint32_t word=0;
    for(unsigned i=0;i<4;++i)word|=uint32_t(bytes[offset+i])<<(i*8);
    return word;
}
inline uint32_t HashWord(uint32_t hash,uint32_t word)
{
    for(unsigned i=0;i<4;++i){hash^=uint8_t(word>>(i*8));hash*=16777619u;}
    return hash;
}
inline std::string Base64(const std::vector<uint8_t>& bytes)
{
    constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;text.reserve((bytes.size()+2)/3*4);
    for(size_t i=0;i<bytes.size();i+=3)
    {
        const size_t left=bytes.size()-i;
        const uint32_t word=(uint32_t(bytes[i])<<16)|(left>1?uint32_t(bytes[i+1])<<8:0)|(left>2?bytes[i+2]:0);
        text.push_back(alphabet[(word>>18)&63]);text.push_back(alphabet[(word>>12)&63]);
        text.push_back(left>1?alphabet[(word>>6)&63]:'=');text.push_back(left>2?alphabet[word&63]:'=');
    }
    return text;
}
inline int Base64Digit(char c)
{
    if(c>='A'&&c<='Z')return c-'A';
    if(c>='a'&&c<='z')return c-'a'+26;
    if(c>='0'&&c<='9')return c-'0'+52;
    return c=='+'?62:(c=='/'?63:-1);
}
inline bool Unbase64(std::string_view text,std::vector<uint8_t>& bytes)
{
    // Even a noncanonical stream with a separate repeated-word token for each
    // value cannot exceed this bound. Check it before allocating or decoding.
    constexpr size_t maxBytes=size_t(MaxResolution)*MaxResolution*8*8+20;
    if(text.empty()||text.size()%4||text.size()>(maxBytes+2)/3*4)return false;
    bytes.clear();bytes.reserve(text.size()/4*3);
    for(size_t i=0;i<text.size();i+=4)
    {
        const int a=Base64Digit(text[i]),b=Base64Digit(text[i+1]);
        const bool pad2=text[i+2]=='=',pad3=text[i+3]=='=';
        const int c=pad2?0:Base64Digit(text[i+2]),d=pad3?0:Base64Digit(text[i+3]);
        if(a<0||b<0||c<0||d<0||(pad2&&!pad3)||((pad2||pad3)&&i+4!=text.size())
            ||(pad2&&(b&15))||(!pad2&&pad3&&(c&3)))return false;
        const uint32_t word=(uint32_t(a)<<18)|(uint32_t(b)<<12)|(uint32_t(c)<<6)|uint32_t(d);
        bytes.push_back(uint8_t(word>>16));
        if(!pad2)bytes.push_back(uint8_t(word>>8));
        if(!pad3)bytes.push_back(uint8_t(word));
    }
    return bytes.size()<=maxBytes;
}
inline Pixel Mix(const Pixel& a,const Pixel& b,float weight)
{
    const float t=1-weight;
    return {{a.color.premultiplied*t+b.color.premultiplied*weight,a.color.coverage*t+b.color.coverage*weight},
        {a.normal.premultiplied*t+b.normal.premultiplied*weight,a.normal.coverage*t+b.normal.coverage*weight}};
}
}

// Mip planes may be smaller than the minimum persistent chart resolution.
inline bool Valid(const Image& image)
{
    using namespace SurfacePaintMath;
    if(!Detail::Dimension(image.width)||!Detail::Dimension(image.height)
        ||image.pixels.size()!=size_t(image.width)*image.height)return false;
    for(const auto& pixel:image.pixels)
    {
        const auto& c=pixel.color;const auto& n=pixel.normal;
        if(!Finite(c.premultiplied)||!std::isfinite(c.coverage)||c.coverage<0||c.coverage>1
            ||!Finite(n.premultiplied)||!std::isfinite(n.coverage)||n.coverage<0||n.coverage>1)return false;
        constexpr float tolerance=2.e-5f;
        if(c.premultiplied.x < -tolerance||c.premultiplied.y < -tolerance||c.premultiplied.z < -tolerance
            ||c.premultiplied.x>c.coverage+tolerance||c.premultiplied.y>c.coverage+tolerance||c.premultiplied.z>c.coverage+tolerance)return false;
        // A convex accumulation of outward unit detail must obey these bounds.
        if(n.premultiplied.z < -tolerance
            ||Dot(n.premultiplied,n.premultiplied)>(n.coverage+tolerance)*(n.coverage+tolerance))return false;
    }
    return true;
}

// Lossless, versioned chart payload. Float bits are written little-endian,
// repeated 32-bit values are run-length encoded, and Base64 makes the bytes
// safe for a serialized prefab string. This is source paint, not a GPU cache.
inline std::string Encode(const Image& image)
{
    if(image.width<MinResolution||image.height<MinResolution||!Valid(image))return {};
    const uint32_t words=uint32_t(image.pixels.size()*8);
    uint32_t hash=2166136261u;
    for(size_t i=0;i<words;++i)hash=Detail::HashWord(hash,Detail::Word(image,i));
    std::vector<uint8_t> bytes;bytes.reserve(size_t(words)*4+24);
    for(uint32_t word:{0x31545053u,image.width,image.height,words,hash})Detail::AddWord(bytes,word);
    size_t i=0;
    while(i<words)
    {
        const auto value=Detail::Word(image,i);size_t end=i+1;
        while(end<words&&Detail::Word(image,end)==value)++end;
        if(end-i>=3)
        {
            Detail::AddWord(bytes,0x80000000u|uint32_t(end-i));Detail::AddWord(bytes,value);i=end;
        }
        else
        {
            const size_t begin=i;i=end;
            while(i<words)
            {
                const auto next=Detail::Word(image,i);end=i+1;
                while(end<words&&Detail::Word(image,end)==next)++end;
                if(end-i>=3)break;i=end;
            }
            Detail::AddWord(bytes,uint32_t(i-begin));
            for(size_t k=begin;k<i;++k)Detail::AddWord(bytes,Detail::Word(image,k));
        }
    }
    return Detail::Base64(bytes);
}
inline bool Decode(std::string_view text,Image& result)
{
    std::vector<uint8_t> bytes;
    if(!Detail::Unbase64(text,bytes)||bytes.size()<20||Detail::ReadWord(bytes,0)!=0x31545053u)return false;
    const auto width=Detail::ReadWord(bytes,4),height=Detail::ReadWord(bytes,8),words=Detail::ReadWord(bytes,12);
    if(width<MinResolution||height<MinResolution||!Detail::Dimension(width)||!Detail::Dimension(height)
        ||words!=size_t(width)*height*8||bytes.size()>size_t(words)*8+20)return false;
    const uint32_t expectedHash=Detail::ReadWord(bytes,16);
    Image image;image.width=width;image.height=height;image.pixels.resize(size_t(width)*height);
    size_t offset=20,index=0;uint32_t hash=2166136261u;
    while(offset<bytes.size()&&index<words)
    {
        if(bytes.size()-offset<4)return false;
        const auto token=Detail::ReadWord(bytes,offset);offset+=4;
        const size_t count=token&0x7fffffffu;
        if(!count||count>words-index)return false;
        if(token&0x80000000u)
        {
            if(bytes.size()-offset<4)return false;
            const auto word=Detail::ReadWord(bytes,offset);offset+=4;
            for(size_t i=0;i<count;++i){Detail::SetWord(image,index++,word);hash=Detail::HashWord(hash,word);}
        }
        else
        {
            if(count>(bytes.size()-offset)/4)return false;
            for(size_t i=0;i<count;++i)
            {
                const auto word=Detail::ReadWord(bytes,offset);offset+=4;
                Detail::SetWord(image,index++,word);hash=Detail::HashWord(hash,word);
            }
        }
    }
    if(index!=words||offset!=bytes.size()||hash!=expectedHash||!Valid(image))return false;
    result=std::move(image);return true;
}

inline Image Downsample(const Image& source)
{
    if(!Valid(source))return {};
    Image image;image.width=std::max(1u,source.width/2);image.height=std::max(1u,source.height/2);
    image.pixels.resize(size_t(image.width)*image.height);
    for(uint32_t y=0;y<image.height;++y)for(uint32_t x=0;x<image.width;++x)
    {
        const uint32_t a=std::min(source.width-1,x*2),b=std::min(source.width-1,x*2+1);
        const uint32_t c=std::min(source.height-1,y*2),d=std::min(source.height-1,y*2+1);
        const auto low=Detail::Mix(source.pixels[size_t(c)*source.width+a],source.pixels[size_t(c)*source.width+b],.5f);
        const auto high=Detail::Mix(source.pixels[size_t(d)*source.width+a],source.pixels[size_t(d)*source.width+b],.5f);
        image.pixels[size_t(y)*image.width+x]=Detail::Mix(low,high,.5f);
    }
    return image;
}
inline std::vector<Image> GenerateMips(const Image& source)
{
    if(!Valid(source))return {};
    std::vector<Image> mips;mips.push_back(source);
    while(mips.back().width>1||mips.back().height>1)mips.push_back(Downsample(mips.back()));
    return mips;
}
inline Pixel Sample(const Image& image,float u,float v)
{
    // Texel centers and clamped edges match a linear texture sampler. Calling
    // code owns chart-domain rejection; clamping must not choose a receiver.
    if(!Detail::Dimension(image.width)||!Detail::Dimension(image.height)||image.pixels.size()!=size_t(image.width)*image.height
        ||!std::isfinite(u)||!std::isfinite(v))return {};
    const float x=std::clamp(u*image.width-.5f,0.f,float(image.width-1));
    const float y=std::clamp(v*image.height-.5f,0.f,float(image.height-1));
    const auto ix=uint32_t(x),iy=uint32_t(y),jx=std::min(image.width-1,ix+1),jy=std::min(image.height-1,iy+1);
    const auto a=Detail::Mix(image.pixels[size_t(iy)*image.width+ix],image.pixels[size_t(iy)*image.width+jx],x-ix);
    const auto b=Detail::Mix(image.pixels[size_t(jy)*image.width+ix],image.pixels[size_t(jy)*image.width+jx],x-ix);
    return Detail::Mix(a,b,y-iy);
}
}

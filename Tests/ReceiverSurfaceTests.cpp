// SPDX-License-Identifier: MIT
// Compile the production CPU geometry with a small O3DE data adapter. Analytic
// positions and normals below independently enforce the renderer's metre contract.
#include "../Include/SilPOM/HeightPrecision.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace AZStd { using std::clamp; }
namespace AZ
{
namespace RHI { enum class Format { R8_UNORM, R16_UNORM, R32_FLOAT, Unsupported }; }
struct Vector2
{
    float x=0,y=0;
    Vector2()=default;
    Vector2(float a,float b):x(a),y(b){}
    float GetX() const{return x;} float GetY() const{return y;}
    Vector2 operator+(Vector2 b) const{return {x+b.x,y+b.y};}
    Vector2 operator-(Vector2 b) const{return {x-b.x,y-b.y};}
    Vector2 operator*(Vector2 b) const{return {x*b.x,y*b.y};}
};
struct Vector3
{
    float x=0,y=0,z=0;
    Vector3()=default;
    Vector3(float a,float b,float c):x(a),y(b),z(c){}
    static Vector3 CreateZero(){return {};}
    float GetX() const{return x;} float GetY() const{return y;} float GetZ() const{return z;}
    Vector3 operator+(Vector3 b) const{return {x+b.x,y+b.y,z+b.z};}
    Vector3 operator-(Vector3 b) const{return {x-b.x,y-b.y,z-b.z};}
    Vector3 operator*(float b) const{return {x*b,y*b,z*b};}
    Vector3 operator/(float b) const{return {x/b,y/b,z/b};}
    float Dot(Vector3 b) const{return x*b.x+y*b.y+z*b.z;}
    Vector3 Cross(Vector3 b) const{return {y*b.z-z*b.y,z*b.x-x*b.z,x*b.y-y*b.x};}
    float GetLengthSq() const{return Dot(*this);}
    Vector3 GetNormalizedSafe() const{return GetLengthSq()>0?*this/std::sqrt(GetLengthSq()):Vector3{};}
};
}
namespace SilPOM
{
struct HeightImage
{
    struct Descriptor
    {
        struct Size {unsigned m_width=2,m_height=2;} m_size;
        AZ::RHI::Format m_format=AZ::RHI::Format::R8_UNORM;
    } descriptor;
    std::vector<uint8_t> pixels;
    const Descriptor& GetImageDescriptor() const{return descriptor;}
    const std::vector<uint8_t>& GetSubImageData(int,int) const{return pixels;}
};
struct HeightAsset
{
    HeightImage* image=nullptr;
    bool IsReady() const{return image!=nullptr;}
    HeightImage* operator->() const{return image;}
};
struct MeshProfileBinding
{
    unsigned m_id=1,m_addressMode=1;
    float m_scale=.4f,m_reference=.5f;
    AZ::Vector2 m_tiling{1,1},m_offset{0,0};
    HeightAsset m_height;
};
struct MeshFace
{
    struct Corner {AZ::Vector3 m_position,m_direction;AZ::Vector2 m_uv;};
    unsigned m_profile=1;
    std::vector<Corner> m_corners;
};
struct MeshConfig
{
    std::vector<MeshProfileBinding> m_profiles;
    float m_reliefDepthMultiplier=1;
    bool m_useR16HeightTextures=false,m_useHeightDerivedNormals=true,trained=false;
    bool UsesTrainedProfiles() const{return trained;}
};
#include "../Source/ReceiverFaceGeometry.inl"
}

void Require(bool value,const char* message)
{if(!value){std::cerr<<message<<'\n';std::exit(1);}}
bool Near(float a,float b,float tolerance=1e-6f){return std::abs(a-b)<tolerance;}
bool Near(AZ::Vector3 a,AZ::Vector3 b,float tolerance=1e-6f)
{return (a-b).GetLengthSq()<tolerance*tolerance;}

int main()
{
    using namespace SilPOM;
    // Exercise every byte of the smallest 8-bit fixture, including its final
    // pixel. The original implementation reads beyond this four-byte payload.
    HeightImage image;image.pixels={0,85,170,255};
    MeshConfig config;config.m_profiles.push_back({});config.m_profiles[0].m_height.image=&image;
    MeshFace face;face.m_corners={{{0,0,0},{0,0,1},{0,0}},{{1,0,0},{0,0,1},{1,0}},{{0,1,0},{0,0,1},{0,1}}};
    ReceiverFaceGeometry geometry;
    Require(geometry.Initialize(face,config,1),"Valid R8 height rejected");
    Require(Near(geometry.Height({.25f,.25f}).GetX(),-.2f),"R8 first pixel decoded incorrectly");
    Require(Near(geometry.Height({.75f,.75f}).GetX(),.2f),"R8 final pixel decoded incorrectly");
    config.m_profiles[0].m_scale=0;
    Require(geometry.Initialize(face,config,1),"Zero-relief receiving face rejected");
    Require(Near(geometry.Height({.75f,.75f}).GetX(),0)&&Near(geometry.Height({.75f,.75f},true).GetX(),1),"Height masks must retain normalized source height at zero relief");
    config.m_profiles[0].m_scale=.4f;
    Require(Near(geometry.Height({.5f,.5f}).GetX(),0),"R8 bilinear interpolation changed");
    Require(Near(geometry.Height({-.25f,1.25f}).GetX(),.4f*(170.f/255-.5f)),"Clamp addressing changed");
    config.m_profiles[0].m_addressMode=0;
    Require(geometry.Initialize(face,config,1),"Repeat R8 height rejected");
    Require(Near(geometry.Height({-.25f,1.25f}).GetX(),.4f*(85.f/255-.5f)),"Repeat addressing changed");
    config.m_profiles[0].m_addressMode=1;

    image.pixels={255,255,255};
    Require(!geometry.Initialize(face,config,1),"Truncated payload accepted");
    image.descriptor.m_format=AZ::RHI::Format::Unsupported;
    Require(!geometry.Initialize(face,config,1),"Unsupported height format accepted");
    image.descriptor.m_format=AZ::RHI::Format::R16_UNORM;
    const uint16_t heights16[]={0,21845,43690,65535};
    image.pixels.resize(sizeof(heights16));std::memcpy(image.pixels.data(),heights16,sizeof(heights16));
    Require(geometry.Initialize(face,config,1),"Valid R16 height rejected");
    Require(Near(geometry.Height({.75f,.75f}).GetX(),.2f),"R16 final pixel decoded incorrectly");

    image.descriptor.m_format=AZ::RHI::Format::R32_FLOAT;
    float heights32[]={.75f,.75f,.75f,.75f};
    image.pixels.resize(sizeof(heights32));std::memcpy(image.pixels.data(),heights32,sizeof(heights32));
    for(float scale:{.5f,1.f,2.f,10.f})
    {
        Require(geometry.Initialize(face,config,scale),"Scaled receiver rejected");
        AZ::Vector3 position,normal;geometry.Evaluate({.25f,.25f},position,normal);
        Require(Near(position,{.25f*scale,.25f*scale,.1f}),"Uniform receiver scale changed world-metre relief");
        Require(Near(normal,{0,0,1}),"Flat-height normal changed with receiver scale");
    }
    // Linear height slope: h(u,v)=.4*(2u-.5) within the central bilinear cell.
    // Normal slope is .8 / receiver scale, while height amplitude stays .1 m.
    const float slope32[]={0,1,0,1};std::memcpy(image.pixels.data(),slope32,sizeof(slope32));
    for(float scale:{.5f,1.f,2.f,10.f})
    {
        Require(geometry.Initialize(face,config,scale),"Sloped scaled receiver rejected");
        AZ::Vector3 position,normal;geometry.Evaluate({.5f,.25f},position,normal);
        Require(Near(position,{.5f*scale,.25f*scale,0}),"Sloped position moved off rendered surface");
        Require(Near(normal,AZ::Vector3(-.8f/scale,0,1).GetNormalizedSafe()),"Height normal ignored base geometry scale");
    }
    config.m_useHeightDerivedNormals=false;
    Require(geometry.Initialize(face,config,2),"Flat shading receiver rejected");
    AZ::Vector3 position,normal;geometry.Evaluate({.5f,.25f},position,normal);
    Require(Near(normal,{0,0,1}),"Disabled height normals still use slope");
    config.m_useHeightDerivedNormals=true;
    config.m_reliefDepthMultiplier=2;
    const float quantized32[]={.734567f,.734567f,.734567f,.734567f};
    std::memcpy(image.pixels.data(),quantized32,sizeof(quantized32));
    config.m_useR16HeightTextures=true;
    Require(geometry.Initialize(face,config,2),"Quantized receiver rejected");
    geometry.Evaluate({.25f,.25f},position,normal);
    const float relief=.4f*(DecodeHeightUnorm16(EncodeHeightUnorm16(quantized32[0]))-.5f);
    Require(Near(position.GetZ(),relief),"CPU heights differ from R32-to-R16 renderer upload");
    config.trained=true;
    Require(geometry.Initialize(face,config,2),"Trained receiver rejected");
    geometry.Evaluate({.25f,.25f},position,normal);
    Require(Near(position.GetZ(),relief*2),"Trained-profile relief multiplier ignored");
    Require(!geometry.Initialize(face,config,0),"Zero receiver scale accepted");
    Require(!geometry.Initialize(face,config,-1),"Negative receiver scale accepted");
    std::cout<<"Receiver format, malformed payload, addressing, scale, slope, quantization and normal settings passed.\n";
}

# SPDX-License-Identifier: MIT
"""Execute production shader traversal with deterministic color/opacity sampling.

The C++ adapter replaces texture fetches with a known texel. Receiver selection,
reference handoff, tree traversal, alpha accumulation and early-out are extracted
from the shipped shader unchanged except for HLSL/C++ syntax adaptation.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"Tools"))
import o3de_patches
REL="Gems/Atom/Feature/Common/Assets/ShaderLib/Atom/Features/PBR/DecalComposite.azsli"


def extract_added_shader(path):
    text=path.read_text()
    marker=f"diff --git a/{REL} b/{REL}\n"
    text=text.split(marker,1)[1].split("\ndiff --git ",1)[0]
    return "".join(line[1:]+"\n" for line in text.splitlines() if line.startswith("+") and not line.startswith("+++"))


def function(text,name):
    start=text.index(name+"(")
    start=text.rfind("\n",0,start)+1
    opening=text.index("{",start);depth=1;end=opening+1
    while depth:
        depth+=(text[end]=="{")-(text[end]=="}");end+=1
    result=text[start:end]
    result=re.sub(r"inout (\w+) (\w+)",r"\1& \2",result)
    result=re.sub(r"\[(?:branch|loop|unroll)\]", "",result)
    return result.replace(".xyz", ".xyz()")


ADAPTER=r'''
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
using uint=unsigned;
struct float3 {float x,y,z;float3(float v=0):x(v),y(v),z(v){} float3(float a,float b,float c):x(a),y(b),z(c){} float3& operator+=(float3 a){x+=a.x;y+=a.y;z+=a.z;return *this;}};
float3 operator+(float3 a,float3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
float3 operator*(float a,float3 b){return {a*b.x,a*b.y,a*b.z};}
float3 operator*(float3 a,float b){return b*a;}
bool operator<(float3 a,float3 b){return a.x<b.x||a.y<b.y||a.z<b.z;}
bool operator>(float3 a,float3 b){return a.x>b.x||a.y>b.y||a.z>b.z;}
bool any(bool b){return b;}
float3 normalize(float3 a){return a;}
float3 lerp(float3 a,float3 b,float t){return (1-t)*a+t*b;}
float saturate(float a){return std::clamp(a,0.f,1.f);}
struct uint3 {uint x,y,z;};
struct uint4 {uint x,y,z,w;uint3 xyz() const{return {x,y,z};}};
float asfloat(uint a){float f;memcpy(&f,&a,4);return f;}
float3 asfloat(uint3 a){return {asfloat(a.x),asfloat(a.y),asfloat(a.z)};}
uint bits(float a){uint n;memcpy(&n,&a,4);return n;}
struct ByteAddressBuffer {std::vector<uint>* data;uint4 Load4(uint offset){auto* p=data->data()+offset/4;return {p[0],p[1],p[2],p[3]};}uint Load(uint offset){return data->at(offset/4);}};
std::vector<uint> bytes;
namespace Bindless {ByteAddressBuffer GetByteAddressBuffer(uint){return {&bytes};}}
struct Decal {uint m_textureArrayIndex=0,m_textureIndex=0;float color=0,alpha=0;};
struct Surface {float3 position=0,albedo=0,normal=1;};
using real3=float3;
uint currentFace=0,currentOwner=0,prefixOwner=0,samples=0;
#define DECAL_PAINT_RECEIVER_FACE() currentFace
#define DECAL_PAINT_RECEIVER_OWNER() currentOwner
#define DECAL_PAINT_HEIGHT() -1.f
#define DECAL_PAINT_PREFIX_OWNED(token) ((token)!=0&&(token)==prefixOwner)
Decal LoadPaintDab(ByteAddressBuffer b,uint o){Decal d;d.color=asfloat(b.Load(o+64));d.alpha=asfloat(b.Load(o+12));return d;}
void SamplePaintAlpha(Decal d,Surface,float3,float3,float3& color,float& alpha){++samples;color=d.color;alpha=d.alpha;}
void ApplyDecalData(Decal d,Surface& s,float3,float3){++samples;s.albedo=lerp(s.albedo,d.color,d.alpha);}
'''
CASES=r'''
struct Dab {float color,alpha;uint owner,flags=2u,layer=0;};
void require(bool b,const char* message){if(!b)throw std::runtime_error(message);}
void fixture(std::vector<Dab> dabs,uint flags,uint fixed=0,uint token=0){
    const uint n=uint(dabs.size()),dabBase=16+n*32;
    bytes.assign((dabBase+n*96+16)/4,0);bytes[0]=n;bytes[1]=dabBase;bytes[2]=flags;bytes[3]=n;
    for(uint i=0;i<n;++i){uint node=4+i*8;bytes[node]=bytes[node+1]=bytes[node+2]=bits(-10);bytes[node+3]=i+1;bytes[node+4]=bytes[node+5]=bytes[node+6]=bits(10);bytes[node+7]=i;
        uint o=(dabBase+i*96)/4;bytes[o+3]=bits(dabs[i].alpha);bytes[o+16]=bits(dabs[i].color);bytes[o+20]=dabs[i].flags|(7u<<8);bytes[o+21]=dabs[i].layer;bytes[o+22]=n;bytes[o+23]=dabs[i].owner;}
    uint end=(dabBase+n*96)/4;bytes[end]=token;bytes[end+1]=fixed;bytes[end+2]=fixed;samples=0;
}
float paint(float base=0){Surface s;s.albedo=base;Decal composite;ApplyPaintComposite(composite,s,0,0);return s.albedo.x;}
int main(){try{
    currentOwner=100;prefixOwner=0;
    for(uint mode:{0u,1u,2u}){
        fixture({{1,.5f,100}},mode);currentFace=10;require(std::abs(paint()-.5f)<1e-6,"stamp center face missing");
        currentFace=11;require(std::abs(paint()-.5f)<1e-6,"receiver footprint clipped at adjacent triangle");
        currentOwner=101;require(paint()==0,"receiver-owned paint leaked to other receiver");currentOwner=100;
        fixture({{1,.5f,10,0}},mode);currentFace=11;require(paint()==0,"UV exact-face prefix spread to other chart");
        currentFace=10;require(std::abs(paint()-.5f)<1e-6,"UV exact-face prefix missing");
    }
    std::vector<Dab> opaque(200,{.2f,.2f,100});opaque[0]={1,1,100};fixture(opaque,1);
    require(paint()==1&&samples==1,"anchored alpha-only paint lost opacity early-out");
    opaque.insert(opaque.begin(),{0,1,101});fixture(opaque,1);
    require(paint()==1&&samples==1,"rejected receiver affected early-out");
    for(uint mode:{0u,2u}){
        prefixOwner=77;fixture({{1,.5f,100}},mode|32u,1,77);
        require(std::abs(paint(.5f)-.5f)<1e-6&&samples==0,"material-owned prefix applied twice");
        fixture({{1,.5f,100},{1,.25f,100}},mode|32u,1,77);
        require(std::abs(paint(.5f)-.625f)<1e-6&&samples==1,"prefix handoff omitted or repeated live tail");
        prefixOwner=0;require(std::abs(paint()-.625f)<1e-6,"unowned prefix must retain full replay");
    }
    std::puts("Native shader: adjacent faces, receiver isolation, alpha early-out and reference ownership passed");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
'''


class NativePaintShader(unittest.TestCase):
    def test_production_traversal(self):
        compiler=shutil.which("clang++")
        if not compiler:self.skipTest("clang++ is required for the portable shader adapter")
        bundle=o3de_patches.locate()
        if bundle is None:self.skipTest("pinned o3de-patches checkout not found; see Integration/README.md")
        # The experimental shader adds bake-field resource sampling; its shared
        # receiver/reference control flow is checked against the stable source.
        shader=extract_added_shader(bundle/"engine.patch")
        experimental=extract_added_shader(bundle/"experiments/paint-bake.patch")
        for name in ("PaintReceiverSelected","ApplyPaintForward"):
            self.assertEqual(function(shader,name),function(experimental,name))
        for text in (shader,experimental):
            traversal=function(text,"ApplyPaintComposite")
            self.assertLess(traversal.index("DECAL_PAINT_PREFIX_OWNED"),traversal.index("if((header.z&2)!=0)"))
            self.assertIn("for(uint i=firstLeaf;",traversal)
            self.assertIn("!PaintReceiverSelected(meta)",traversal)
        source=ADAPTER+"\n"+"\n".join(function(shader,name) for name in ("PaintReceiverSelected","PaintHeightSelected","ApplyPaintForward","ApplyPaintComposite"))+"\n"+CASES
        with tempfile.TemporaryDirectory(prefix="silpom-native-shader-") as temporary:
            folder=Path(temporary);cpp=folder/"test.cpp";exe=folder/"test.exe";cpp.write_text(source)
            compiled=subprocess.run([compiler,"-std=c++17",str(cpp),"-o",str(exe)],capture_output=True,text=True)
            self.assertEqual(compiled.returncode,0,compiled.stdout+compiled.stderr)
            ran=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)
            print(ran.stdout.strip())


if __name__=="__main__":unittest.main()

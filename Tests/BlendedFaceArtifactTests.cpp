// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/FaceNormals.h"
#include "BlendedFaceCpu.h"
#include <array>
#include <iostream>
#include <cstdlib>
using namespace SilPOM::FaceNormals;
using namespace SilPOM::Cpu;
void Require(bool b,const char* message) { if(!b) {std::cerr<<message<<'\n';std::exit(1);} }
struct Triangle {float3 a,b,c;};
bool TriangleHit(SilPomRay ray,Triangle tr,float& t)
{
    auto e=tr.b-tr.a,g=tr.c-tr.a,h=cross(ray.direction,g);
    float det=dot(e,h);if(std::abs(det)<1e-10f)return false;
    auto s=ray.origin-tr.a;float u=dot(s,h)/det;if(u<0||u>1)return false;
    auto q=cross(s,e);float v=dot(ray.direction,q)/det;if(v<0||u+v>1)return false;
    t=dot(g,q)/det;return t>=ray.tMin&&t<=ray.tMax;
}
int main()
{
    std::vector<Face> input;
    Vector vertices[9];for(int z=0;z<3;++z)for(int x=0;x<3;++x)vertices[z*3+x]={double(x-1),.6*std::max(x-1,0),double(z-1)};
    unsigned indices[8][3]={{0,1,4},{0,4,3},{1,2,5},{1,5,4},{3,4,7},{3,7,6},{4,5,8},{4,8,7}};
    for(int i=0;i<8;++i){Face f;f.tagged=i>=2;for(int c=0;c<3;++c){f.vertex[c]=indices[i][c];f.position[c]=vertices[indices[i][c]];}input.push_back(f);}
    auto normals=Build(input);Require(normals.error.empty(),normals.error.c_str());
    Texture texture{64,64,{}};
    for(int y=0;y<64;++y)for(int x=0;x<64;++x)texture.pixels.push_back(.5f+.32f*std::sin(6.2831853f*(x+.5f)/16)*std::cos(6.2831853f*(y+.5f)/16));
    struct Surface{SilPomMeshFace f;SilPomPatch p;float2 low;std::vector<Triangle> reference;};
    std::vector<Surface> surfaces;
    auto vec=[](Vector v){return float3(float(v.x),float(v.y),float(v.z));};
    for(int fi=2;fi<8;++fi)
    {
        Surface s;auto& f=s.f;
        f.origin=vec(input[fi].position[0]);f.du={2,fi<4||fi>=6?1.2f:0,0};f.dv={0,0,2};f.normal=normalize(cross(f.du,f.dv));
        auto uv=[](Vector v){return float2(float(v.x+1)*.5f,float(v.z+1)*.5f);};
        f.uv0=uv(input[fi].position[0]);f.uv1=uv(input[fi].position[1]);f.uv2=uv(input[fi].position[2]);
        f.d0=vec(normals.directions[fi][0]);f.d1=vec(normals.directions[fi][1]);f.d2=vec(normals.directions[fi][2]);
        s.low={std::min({f.uv0.x,f.uv1.x,f.uv2.x}),std::min({f.uv0.y,f.uv1.y,f.uv2.y})};
        s.p={.5f,.5f,.3f,.5f,.5f,.5f,s.low.x+.035f,s.low.y+.02f,64,64,0,4096};
        auto position=[&](float a,float b){float2 q{f.uv0.x+(f.uv1.x-f.uv0.x)*a+(f.uv2.x-f.uv0.x)*b,f.uv0.y+(f.uv1.y-f.uv0.y)*a+(f.uv2.y-f.uv0.y)*b};float h=SilPomFaceHeight(texture,s.p,q,s.low).x;return f.origin+f.du*(q.x-f.uv0.x)+f.dv*(q.y-f.uv0.y)+SilPomFaceDirection(f,q)*h;};
        const int n=128;
        for(int j=0;j<n;++j)for(int i=0;i<n-j;++i){auto a=position(i/float(n),j/float(n)),b=position((i+1)/float(n),j/float(n)),c=position(i/float(n),(j+1)/float(n));s.reference.push_back({a,b,c});if(i+j+1<n)s.reference.push_back({b,position((i+1)/float(n),(j+1)/float(n)),c});}
        surfaces.push_back(std::move(s));
    }
    // Pixel rays reproduced from the oblique and near-plane fixture views.
    // Each formerly produced SP_EXHAUSTED with fewer than 60 cells visited.
    // An independent dense triangle reference confirms these rays miss.
    struct Case {int view,x,y,surface;};
    const Case cases[]={{0,115,47,2},{0,110,54,2},{0,115,63,3},
                        {1,240,64,3},{1,239,65,3},{1,240,65,3}};
    for(auto sample:cases)
    {
        int view=sample.view,x=sample.x,y=sample.y;
        float3 origin=view?float3(-.5f,-.2f,.5f):float3(2.5f,-2.5f,.2f);
        float3 forward=view?float3(0,1,0):normalize(float3(-1,1,0)),right={forward.y,-forward.x,0};
        SilPomRay ray{origin,normalize(forward+right*((2*(x+.5f)/256-1)*1.0264f)+float3(0,0,(1-2*(y+.5f)/144)*.57735f)),.1f,100};
        auto& s=surfaces[sample.surface];float t=0;
        for(auto tr:s.reference)Require(!TriangleHit(ray,tr,t),"Reference geometry contradicts expected silhouette miss");
        auto hit=SilPomIntersectFace(texture,s.p,s.f,ray,s.low);
        Require(hit.status==SP_MISS,"Rejected planar seed became diagnostic geometry");
        auto limited=s.p;limited.maxCells=1;
        auto exhausted=SilPomIntersectFace(texture,limited,s.f,ray,s.low);
        Require(exhausted.status==SP_EXHAUSTED,"Actual traversal exhaustion must remain distinguishable from seed rejection");
    }
    std::cout<<"Six silhouette/near-plane miss regressions agree with dense reference; real budget exhaustion retained\n";
}

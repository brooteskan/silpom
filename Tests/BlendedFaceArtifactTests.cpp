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
    // Adjacent turns must meet without the former forced-zero helical stripe.
    {
        Require(SilPomSpiralTurnBlend(0)==0 && SilPomSpiralTurnBlend(1)==1
            && SilPomSpiralTurnBlend(.5f)==.5f,"Spiral turn blend must join at its endpoints and midpoint");
        const auto lightAt=[](float angle,float height)
        {
            const float lower=angle+floor(height-angle);
            const float blend=SilPomSpiralTurnBlend(height-lower);
            float lowerLux=0,upperLux=0;
            for(int w=-4;w<=4;++w)
            {
                lowerLux=std::max(lowerLux,SilPomSpiralWindowLux(lower,float(w),1));
                upperLux=std::max(upperLux,SilPomSpiralWindowLux(lower+1,float(w),1));
            }
            return lowerLux+(upperLux-lowerLux)*blend;
        };
        for(int a=0;a<=100;++a)
        {
            const float angle=-.5f+float(a)*.01f;
            for(int h=0;h<=400;++h)
            {
                const float height=-2.0f+float(h)*.01f;
                Require(lightAt(angle,height)>=.4999f,"Overlapping lit turns must not develop an artificial black band");
            }
            for(float boundary:{angle,angle+.5f,angle+1})
                Require(std::abs(lightAt(angle,boundary-1e-5f)-lightAt(angle,boundary+1e-5f))<1e-4f,
                    "Spiral illumination must remain continuous at half- and whole-turn boundaries");
        }
        for(float height:{-.75f,0.0f,.75f})
            Require(std::abs(lightAt(-.5f,height)-lightAt(.5f,height))<1e-6f,
                "The angular wrap must not introduce another spiral seam");
        Require(SilPomSpiralWindowLux(2,0,1)==0 && SilPomSpiralWindowLux(0,0,0)==0,
            "Turn blending must retain finite window reach and respect disabled windows");
    }
    // The shared fade/gain applies linearly to first and additional windows.
    {
        Require(SilPomSpiralLightGain(21,24,2,1)==1 && SilPomSpiralLightGain(24,24,2,1)==0,
            "Top fade must be full below its start and zero at its end");
        Require(SilPomSpiralLightGain(23,24,2,1)==.5f,"Top fade midpoint must be smooth half brightness");
        float previous=1;
        for(uint i=0;i<=200;++i)
        {
            const float z=22.0f+float(i)*.01f;
            const float gain=SilPomSpiralLightGain(z,24,2,1);
            Require(gain>=0 && gain<=previous+1e-6f,"Top fade must decrease continuously without bright bands");
            Require(std::abs(SilPomSpiralLightGain(z,24,2,2)-2*gain)<1e-6f,
                "Master brightness must scale the whole fade linearly");
            previous=gain;
        }
        Require(SilPomSpiralLightGain(22.4312485f,24,2,1)>0.8f,
            "The uppermost authored window must no longer be clipped by the old 20 m limit");
        Require(SilPomSpiralLightGain(20,24,0,3)==3 && SilPomSpiralLightGain(100,24,0,3)==3,
            "Zero fade distance must disable the height limit");
        Require(SilPomSpiralLightGain(20,24,2,0)==0,"Zero brightness must mute all windows");
    }
    // Defaults preserve the old LOD cutoff; an explicit cap includes overlap.
    {
        const float baseline=.1f*.95f*1.8f*328.0f/4.0f;
        Require(std::abs(SilPomQualityLodDepth(.1f,.05f,1.8f,4,0,0)-baseline)<1e-5f,
            "Default quality settings must preserve the existing LOD cutoff");
        const float cheaper=SilPomQualityLodDepth(.1f,.05f,1.8f,4,8,0);
        Require(std::abs(cheaper-baseline*.5f)<1e-5f,"Doubling pixel error budget must halve relief cutoff");
        for(float cap:{.01f,.1f,1.0f,10.0f})
        {
            const float cutoff=SilPomQualityLodDepth(.1f,.05f,1.8f,4,0,cap);
            const float overlap=SilPomQualityLodOverlap(.1f,cap);
            Require(cutoff>=overlap && cutoff+overlap<=cap+1e-6f,
                "A short distance cap must retain shell overlap and bound the near draw");
        }
        Require(SilPomQualityLodDepth(0,.05f,1.8f,4,0,0)==0,"Flat relief must use shell only");
        Require(SilPomValidationDistanceBlend(100,0,0)==0,"Zero fade end must disable distance quality");
        Require(SilPomValidationDistanceBlend(5,10,30)==0 && SilPomValidationDistanceBlend(40,10,30)==1,
            "Distance quality must clamp to its endpoints");
        const float midpoint=SilPomValidationDistanceBlend(20,10,30);
        Require(midpoint==.5f && SilPomDistanceBudget(6,4,midpoint)==5,
            "Iteration budgets must interpolate through the fade");
        Require(SilPomDistanceBudget(4,0,1)==0 && SilPomDistanceBudget(1,16,0)==1,
            "Distance budgets must retain zero retries and exact near defaults");
    }
    // A nearly converged seed distinguishes the work cap from the acceptance tolerance.
    {
        Texture flat{4,4,std::vector<float>(16,.5f)};
        SilPomPatch p{};
        p.width=p.height=p.tileU=p.tileV=1;
        p.scale=.1f;p.reference=.5f;p.textureWidth=p.textureHeight=4;
        SilPomMeshFace face{};
        face.du={1,0,0};face.dv={0,1,0};face.normal=face.d0=face.d1=face.d2={0,0,1};
        face.uv1={1,0};face.uv2={0,1};
        const auto baseNormal=SilPomShadingNormal(flat,p,face,{.25f,.25f},{0,0},false);
        Require(flat.loads==0 && baseNormal.z==1,"Base-normal quality must avoid all height texture reads");
        SilPomShadingNormal(flat,p,face,{.25f,.25f},{0,0},true);
        Require(flat.loads==4,"Height-derived normal must read a bilinear cell");
        flat.loads=0;
        SilPomRay ray{{.25f,.25f,1},{0,0,-1},0,2};
        float2 uv{.25f,.25f};float t=1.00001f;
        Require(SilPomRefineFace(flat,p,face,ray,{0,0},uv,t,1,1)==SP_REFINE_REJECTED,
            "An iteration cap must not accept an unchecked final correction");
        Require(flat.loads==4,"One iteration must evaluate only one bilinear height");
        uv={.25f,.25f};t=1.00001f;flat.loads=0;
        Require(SilPomRefineFace(flat,p,face,ray,{0,0},uv,t,2,1)==SP_HIT && flat.loads==8,
            "An additional iteration must validate the corrected intersection");
        uv={.25f,.25f};t=1.00001f;flat.loads=0;
        Require(SilPomRefineFace(flat,p,face,ray,{0,0},uv,t,1,10)==SP_HIT && flat.loads==4,
            "Looser tolerance must permit earlier acceptance of a close seed");
        ray.origin={.75f,.75f,1};uv={.75f,.75f};t=1;
        Require(SilPomRefineFace(flat,p,face,ray,{0,0},uv,t,4,100)==SP_MISS,
            "Relaxed validation must retain triangle containment");
    }
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
    struct Case {int view,x,y,surface; bool sweptMiss;};
    const Case cases[]={{0,115,47,2,true},{0,110,54,2,true},{0,115,63,3,false},
                        {1,240,64,3,true},{1,239,65,3,true},{1,240,65,3,true}};
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
        const auto before=texture.loads;
        auto exhausted=SilPomIntersectFace(texture,limited,s.f,ray,s.low);
        if(sample.sweptMiss)
            Require(exhausted.status==SP_MISS && texture.loads==before,"Swept face miss performed heightfield work");
        else
            Require(exhausted.status==SP_EXHAUSTED,"A ray entering the bound still reports actual traversal exhaustion");
    }
    std::cout<<"Six silhouette/near-plane miss regressions agree with dense reference; real budget exhaustion retained\n";
}

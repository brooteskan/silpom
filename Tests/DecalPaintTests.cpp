// SPDX-License-Identifier: MIT
// The Editor's unity build can include Qt before these headers. Its empty
// `emit` macro must not erase a sampler callback invocation.
#define emit
#include "../Include/SilPOM/DecalPaintMath.h"
#include "../Include/SilPOM/DecalCompositeMath.h"
#include "../Include/SilPOM/DecalBakeMath.h"
#undef emit
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace SilPOM::PaintMath;
void Require(bool condition,const char* message) {if(!condition){std::fprintf(stderr,"%s\n",message);std::exit(1);}}
std::vector<Point> Samples(const std::vector<Point>& path,bool pressure=false,size_t limit=10000)
{
    std::vector<Point> result;
    Sample(path.size(),[&](size_t i){return path[i];},[&](Point p){result.push_back(p);return true;},1,.25f,pressure,.1f,limit);return result;
}
int main()
{
    for(auto span:{Vec{.01f,1,4},Vec{20,20,20},Vec{.2f,.2f,.2f}})
    {
        const Bounds box{{-1,-2,-3},span-Vec{1,2,3}};
        const auto grid=MakeBakeGrid(box,128,4*1024*1024);
        Require(grid.Bytes()<=4*1024*1024&&grid.x>=2&&grid.y>=2&&grid.z>=2,"Baked fields obey their image-byte budget");
        Require(grid.width==(grid.x+2)*grid.columns&&grid.height>=(grid.y+2)*(grid.z*4/grid.columns),"Height variants and slice gutters fit their atlas");
    }
    {
        const auto strip=MakeBakeGrid(Bounds{{0,0,0},{3,.01f,.01f}},128,4*1024*1024,1);
        Require(strip.x>128&&strip.Bytes()<=4*1024*1024,"Long thin paint fields retain their requested density without wasting it on square atlas tiles");
    }
    Require(BakeBlendSupported(0)&&BakeBlendSupported(1)&&BakeBlendSupported(3)&&!BakeBlendSupported(2)&&!BakeBlendSupported(4),"Non-affine modes retain projection fallback");
    for(float original:{0.f,.17f,.5f,1.f})
    {
        float forward=original,scale=1,bias=0;
        for(unsigned i=0;i<300;++i)
        {
            const float a=float((i*7)%31)/31,c=float((i*11)%29)/29;
            const unsigned mode=i%3==0?0:(i%3==1?1:3);const bool erase=i%13==0;
            const float target=erase?original:(mode==1?forward*c:(mode==3?1-(1-forward)*(1-c):c));
            forward=forward*(1-a)+target*a;BakeColor(c,a,mode,erase,scale,bias);
        }
        Require(std::abs(forward-(scale*original+bias))<1e-5f,"Baked affine color preserves mixed operation and erase order");
    }
    float bound=2;for(unsigned i=0;i<20;++i)bound=NormalDependence(bound,.8f);
    Require(bound<.005f&&bound>.002f,"Dense normals converge with rounding allowance");
    for(auto start:{Vec{1,0,0},Vec{-1,0,0},Vec{0,1,0},Vec{0,-1,0},Vec{0,0,1},Vec{0,0,-1}})
    {
        Vec a=start,b{0,0,1};float u=2;
        for(unsigned i=0;i<20;++i)
        {
            const Vec target=Vec{float(i%3)*.3f,.2f,1}.Normalized();
            a=(a*.2f+target*.8f).Normalized();b=(b*.2f+target*.8f).Normalized();u=NormalDependence(u,.8f);
            Require((a-b).Length()<=u,"Normal convergence bound covers differing incoming base normals");
        }
    }
    Require(NormalDependence(2,.5f)==2&&NormalDependence(2,1)==0,"Uncertain half blends fall back; full normal replacement is independent");
    for(auto start:{Vec{1,0,0},Vec{-1,0,0},Vec{0,1,0},Vec{0,0,-1}})
    for(float nominal:{.2f,.499f,.5f,.8f,.9999f})
    {
        Vec a=start,b{0,0,1};float u=2;
        const float low=std::max(0.f,nominal-.0001f),high=std::min(1.f,nominal+.0001f);
        for(unsigned i=0;i<50;++i)
        {
            const Vec target=Vec{float(i%3)*.3f,.2f,1}.Normalized();
            const float actual=i%2?low:high;
            a=(a*(1-actual)+target*actual).Normalized();b=(b*(1-nominal)+target*nominal).Normalized();
            u=NormalIntervalDependence(u,nominal,low,high);
            Require((a-b).Length()<=u+1e-6f,"Normal interval bound covers different receiver-angle attenuation and base normals");
        }
    }
    for(float alpha:{.2f,.4f,.5f,.8f})
    for(unsigned axis=0;axis<12;++axis)
    {
        const float angle=float(axis)*.5235988f;
        Vec a{std::cos(angle)*.7f,std::sin(angle)*.7f,.7141428f},b{0,0,1};float u=.765367f;
        for(unsigned i=0;i<100;++i)
        {
            const float turn=float(i)*.731f;
            const Vec target{std::cos(turn)*.7f,std::sin(turn)*.7f,.7141428f};
            const float actual=alpha+(i%2?-.0001f:.0001f);
            a=(a*(1-actual)+target*actual).Normalized();b=(b*(1-alpha)+target*alpha).Normalized();
            u=NormalConeDependence(u,alpha,alpha-.0001f,alpha+.0001f);
            Require((a-b).Length()<=u+1e-6f,"Validated hemisphere cone covers weak-opacity normal chains and receiver-angle uncertainty");
        }
        if(alpha>=.5f)Require(u<.01f,"Dense moderate-opacity paint can certify its normal cache without accepting opposite-facing base normals");
    }
    for(unsigned split=1;split<80;++split)
    {
        float forward=.3f,scale=1,bias=0;
        for(unsigned i=0;i<split;++i)
        {
            float a=float((i*7)%19)/19,c=float((i*13)%17)/17;bool erase=i%11==0;
            forward=forward*(1-a)+(erase?.3f:c)*a;BakeColor(c,a,0,erase,scale,bias);
        }
        for(unsigned i=split;i<100;++i)
        {float a=float((i*7)%19)/19,c=float((i*13)%17)/17;forward=forward*(1-a)+(i%11==0?.3f:c)*a;}
        float color=0,alpha=0,weight=1;
        for(unsigned i=100;i-->split;)
        {
            float a=float((i*7)%19)/19,c=float((i*13)%17)/17;
            if(i%11!=0){color+=weight*a*c;alpha+=weight*a;}weight*=1-a;
        }
        color+=weight*bias;alpha+=weight*(1-scale);
        Require(std::abs(forward-(color+(1-alpha)*.3f))<1e-5f,"Baked fixed prefix composes with reverse live alpha and layer erase");
    }
    for(unsigned i=0;i<100;++i)
    {
        const float x=(i+.5f)/100;Bounds thin{{x,-.001f,-.001f},{x+.00001f,.001f,.001f}};
        for(float corner:{0.f,1.f})Require(Bounds{{corner-1,-1,-1},{corner+1,1,1}}.Intersects(thin),"Conservative bake neighbourhoods cannot drop thin stamps");
    }
    // Spatial grouping must yield exactly the original ordered stream at every
    // point, including split planes and dabs spanning many cells. This protects
    // every blend/normal/erase/layer operation, not just commutative alpha.
    std::vector<Bounds> spatial;
    for(int i=0;i<4096;++i)
    {
        const float x=float((i*37)%127)-63,y=float((i*71)%131)-65,z=float((i*43)%137)-68;
        spatial.push_back({{x-.7f,y-.4f,z-.3f},{x+.7f,y+.4f,z+.3f}});
    }
    spatial.push_back({{-80,-80,-80},{80,80,80}});
    const auto groups=BuildSpatialGroups(spatial);
    Require(groups.size()>1&&groups.size()<=32,"Spatial partition bounded native handle count");
    size_t refs=0;for(const auto& g:groups)refs+=g.projections.size();
    Require(refs<=spatial.size()*2,"Cross-cell references bounded to twice the input");
    auto check=[&](Vec point)
    {
        std::vector<uint32_t> expected,actual;unsigned owners=0;
        for(uint32_t i=0;i<spatial.size();++i)if(spatial[i].Contains(point))expected.push_back(i);
        for(const auto& g:groups)if(g.Contains(point))
        {
            ++owners;
            std::vector<CompositeNode> local;
            BuildCompositeNodes(local,0,uint32_t(g.projections.size()),[&](uint32_t i){return Intersection(spatial[g.projections[i]],g.clip);});
            VisitComposite(local,point,[&](uint32_t i){actual.push_back(g.projections[i]);return true;});
        }
        Require(owners==1&&actual==expected,"Exclusive cell ownership preserves exact ordered candidates");
    };
    for(const auto& b:spatial){check(b.low);check(b.high);check((b.low+b.high)*.5f);}
    for(const auto& g:groups)
    {
        check(g.clip.low);check(g.clip.high);check((g.clip.low+g.clip.high)*.5f);
        for(unsigned axis=0;axis<3;++axis)
        {
            Vec p=(g.clip.low+g.clip.high)*.5f;SetAxis(p,axis,Axis(g.clip.high,axis));check(p);
            SetAxis(p,axis,std::nextafter(Axis(g.clip.high,axis),-INFINITY));check(p);
            SetAxis(p,axis,std::nextafter(Axis(g.clip.high,axis),INFINITY));
            if(Bounds{{-80,-80,-80},{80,80,80}}.Contains(p))check(p);
        }
    }
    Require(BuildSpatialGroups({}).empty(),"Empty paint has no spatial volumes");
    Require(BuildSpatialGroups(spatial,false).size()==1,"Single-volume benchmark baseline retained");
    Require(BuildSpatialGroups(std::vector<Bounds>(5000,{{-1,-1,-1},{1,1,1}})).size()==1,"Coincident paint stays in one volume");
    Require(BrushFlags(true,5)==0x501&&BrushFlags(false,7)==0x700,"GPU operation and height-band packing");
    for(unsigned mask=0;mask<8;++mask)
    {
        Require(HeightSelected(mask,.1f)==bool(mask&1)&&HeightSelected(mask,.5f)==bool(mask&2)&&HeightSelected(mask,.9f)==bool(mask&4),"Independent combinable height ranges");
        Require(HeightSelected(mask,-1)==(mask==7),"Unknown height requires all ranges");
    }
    Require(HeightSelected(2,1.f/3.f)&&!HeightSelected(1,1.f/3.f)&&HeightSelected(4,2.f/3.f)&&HeightSelected(4,1),"Height band endpoints partition without overlap");
    // Compare ordered paint/erase against its independently accumulated affine
    // layer transfer. Dense paint must erase as a whole, not one dab at a time.
    struct Op{int layer;float color,alpha;bool erase;unsigned mask;};
    std::vector<Op> ops;
    for(int layer=0;layer<3;++layer)for(int i=0;i<300;++i)
        ops.push_back({layer,float((i*31)%97)/97.f,float((i*13)%101)/100.f,i%7==0,unsigned(i%8)});
    ops.push_back({2,0,1,true,7});ops.push_back({2,.8f,.35f,false,7});
    for(float height:{-1.f,.1f,.5f,.9f})
    {
        const float original=.27f;float forward=original,base=original;int layer=-1;
        for(const auto& op:ops)
        {
            if(layer!=op.layer){layer=op.layer;base=forward;}
            if(HeightSelected(op.mask,height))forward=forward*(1-op.alpha)+(op.erase?base:op.color)*op.alpha;
        }
        float reverse=0,transmittance=1;
        for(int l=2;l>=0;--l)
        {
            float paint=0,alpha=0,weight=1;
            for(auto it=ops.rbegin();it!=ops.rend();++it)if(it->layer==l&&HeightSelected(it->mask,height))
            {if(!it->erase){paint+=weight*it->alpha*it->color;alpha+=weight*it->alpha;}weight*=1-it->alpha;}
            reverse+=transmittance*paint;transmittance*=std::max(0.f,1-alpha);
        }
        reverse+=transmittance*original;
        Require(std::abs(forward-reverse)<1.e-5f,"Erase preserves lower layers and later paint under dense overlap");
    }
    Require(Samples({}).empty(),"Empty stroke");
    Require(Samples({Point{}}).size()==1,"Single stamp");
    const auto sparse=Samples({{{0,0,0}},{{2,0,0}}});
    std::vector<Point> dense;for(int i=0;i<=40;++i)dense.push_back({{i*.05f,0,0}});
    const auto frequent=Samples(dense);
    Require(sparse.size()==9&&frequent.size()==sparse.size(),"Spacing independent of input rate");
    for(size_t i=0;i<sparse.size();++i)Require((sparse[i].position-frequent[i].position).Length()<1.e-5f,"Stable positions");
    Require(Samples({{{0,0,0}},{{0,0,0}},{{0,0,0}}}).size()==1,"No accumulation while stationary");
    const auto gap=Samples({{{0,0,0}},{{.5f,0,0}},{{10,0,0},{0,0,1},1,true},{{10.5f,0,0}}});
    Require(gap.size()==6,"No bridging a gap");
    for(auto p:gap)Require(p.position.x<=.5f||p.position.x>=10,"No hidden samples between surfaces");
    const auto bend=Samples({{{0,0,0}},{{.5f,0,0}},{{.5f,.5f,0}}});
    Require(bend.size()==5&&std::abs(bend.back().position.y-.5f)<1.e-5f,"Arc length round corner");
    const auto pressure=Samples({{{0,0,0},{0,0,1},.25f},{{1,0,0},{0,1,1},.25f}},true);
    Require(pressure.size()==17,"Pressure alters spacing with footprint size");
    for(auto p:pressure)Require(std::abs(p.normal.Length()-1)<1.e-5f,"Normals remain unit length");
    Require(Samples({{{0,0,0}},{{1000,0,0}}},false,32).size()==32,"Bounded rendering at capacity");
    Require(Pressure(std::numeric_limits<float>::quiet_NaN())==0&&Pressure(2)==1&&Pressure(-1)==0,"Pressure clamps driver noise");
    Require(std::abs(Diameter(2,0,true,.1f)-.2f)<1.e-6f,"Nonzero minimum pen size");
    Require(Diameter(2,.1f,false,.1f)==2,"Mouse unaffected by pressure");
    // Incremental sampling retains its phase when an editor drops old dabs.
    StrokeSampler sampler;std::vector<Point> tail,incremental;
    for(const auto& p:dense)sampler.Push(p,[&](Point dab){incremental.push_back(dab);tail.push_back(dab);if(tail.size()>4)tail.erase(tail.begin());},1,.25f,false,.1f);
    Require(incremental.size()==frequent.size()&&tail.size()==4,"Rolling history does not restart spacing");
    for(size_t i=0;i<incremental.size();++i)Require((incremental[i].position-frequent[i].position).Length()<1.e-5f,"Incremental path matches original sampler");
    // Thousands of ordered projections must produce exactly the same candidate
    // stream as a brute-force scan, including overlapping and disjoint boxes.
    std::vector<Bounds> boxes;
    for(int i=0;i<5000;++i){const float x=float((i*37)%211)*.1f;boxes.push_back({{x,-.5f,-.5f},{x+.75f,.5f,.5f}});}
    std::vector<CompositeNode> nodes;
    BuildCompositeNodes(nodes,0,uint32_t(boxes.size()),[&](uint32_t i){return boxes[i];});
    Require(nodes.size()==9999&&nodes.front().next==nodes.size(),"Composite supports more than old dab limit");
    for(int sample=0;sample<220;++sample)
    {
        const Vec p{sample*.1f,.1f,0};std::vector<uint32_t> expected,actual;
        for(uint32_t i=0;i<boxes.size();++i)if(boxes[i].Contains(p))expected.push_back(i);
        VisitComposite(nodes,p,[&](uint32_t i){actual.push_back(i);return true;});
        Require(actual==expected,"Spatial skipping preserves blend order");
    }
    std::puts("Decal paint spacing, gaps, normals, pressure and capacity passed");
}

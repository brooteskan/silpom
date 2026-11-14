// SPDX-License-Identifier: MIT
// The Editor's unity build can include Qt before these headers. Its empty
// `emit` macro must not erase a sampler callback invocation.
#define emit
#include "../Include/SilPOM/DecalPaintMath.h"
#include "../Include/SilPOM/DecalCompositeMath.h"
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

// SPDX-License-Identifier: MIT
#pragma once
#include "DecalPaintMath.h"
#include <vector>
#include <cstdint>
#include <limits>

namespace SilPOM::PaintMath
{
struct Bounds
{
    Vec low{INFINITY,INFINITY,INFINITY},high{-INFINITY,-INFINITY,-INFINITY};
    void Add(Vec p)
    {
        low={std::min(low.x,p.x),std::min(low.y,p.y),std::min(low.z,p.z)};
        high={std::max(high.x,p.x),std::max(high.y,p.y),std::max(high.z,p.z)};
    }
    void Add(const Bounds& b){Add(b.low);Add(b.high);}
    bool Contains(Vec p) const
    {return p.x>=low.x&&p.y>=low.y&&p.z>=low.z&&p.x<=high.x&&p.y<=high.y&&p.z<=high.z;}
    bool Intersects(const Bounds& b) const
    {return low.x<=b.high.x&&high.x>=b.low.x&&low.y<=b.high.y&&high.y>=b.low.y&&low.z<=b.high.z&&high.z>=b.low.z;}
};
inline float Axis(Vec v,unsigned axis){return axis==0?v.x:(axis==1?v.y:v.z);}
inline void SetAxis(Vec& v,unsigned axis,float value){if(axis==0)v.x=value;else if(axis==1)v.y=value;else v.z=value;}
inline Bounds Intersection(const Bounds& a,const Bounds& b)
{
    return {{std::max(a.low.x,b.low.x),std::max(a.low.y,b.low.y),std::max(a.low.z,b.low.z)},
        {std::min(a.high.x,b.high.x),std::min(a.high.y,b.high.y),std::min(a.high.z,b.high.z)}};
}
inline float Area(const Bounds& b)
{
    const auto d=b.high-b.low;return 2*(d.x*d.y+d.y*d.z+d.z*d.x);
}
struct SpatialGroup
{
    Bounds clip,bounds;
    // Internal split planes belong only to their upper child. Root outer
    // faces stay inclusive. Cells therefore never composite a point twice.
    uint32_t inclusiveHigh=7;
    std::vector<uint32_t> projections;
    bool Contains(Vec p) const
    {
        if(!clip.Contains(p))return false;
        for(unsigned a=0;a<3;++a)if(!(inclusiveHigh&(1u<<a))&&Axis(p,a)>=Axis(clip.high,a))return false;
        return true;
    }
};
// Bounded spatial BSP, independent of edit history and material identity. Each
// leaf retains the incoming sequence; cross-plane projections are references
// in both leaves, evaluated only in the point's exclusive cell. Surface-area
// cost rejects dense/coincident paint and caps both native handles and memory.
inline std::vector<SpatialGroup> BuildSpatialGroups(const std::vector<Bounds>& boxes,bool enabled=true)
{
    if(boxes.empty())return {};
    SpatialGroup root;
    for(uint32_t i=0;i<boxes.size();++i){root.clip.Add(boxes[i]);root.projections.push_back(i);}
    root.bounds=root.clip;
    std::vector<SpatialGroup> groups{root};
    struct Candidate
    {
        bool ready=false;
        float gain=0;
        size_t extra=0;
        SpatialGroup left,right;
    };
    std::vector<Candidate> candidates(1);
    size_t references=boxes.size();
    while(enabled&&groups.size()<32)
    {
        float bestGain=0;size_t best=groups.size();
        for(size_t g=0;g<groups.size();++g)
        {
            const auto& parent=groups[g];if(parent.projections.size()<=128)continue;
            auto& candidate=candidates[g];
            // An unchanged leaf's split cost is unchanged. Recompute only if
            // the shrinking global duplication budget invalidates its choice.
            if(!candidate.ready||references+candidate.extra>boxes.size()*2)
            {
                candidate={};candidate.ready=true;
                const float parentCost=Area(parent.bounds)*float(parent.projections.size());
                for(unsigned axis=0;axis<3;++axis)
                {
                    std::vector<float> centers;centers.reserve(parent.projections.size());
                    for(auto i:parent.projections)centers.push_back((Axis(boxes[i].low,axis)+Axis(boxes[i].high,axis))*.5f);
                    std::sort(centers.begin(),centers.end());
                    for(unsigned quartile=1;quartile<=3;++quartile)
                    {
                        const float split=centers[centers.size()*quartile/4];
                        if(split<=Axis(parent.clip.low,axis)||split>=Axis(parent.clip.high,axis))continue;
                        SpatialGroup left,right;left.clip=right.clip=parent.clip;
                        SetAxis(left.clip.high,axis,split);SetAxis(right.clip.low,axis,split);
                        left.inclusiveHigh=parent.inclusiveHigh&~(1u<<axis);right.inclusiveHigh=parent.inclusiveHigh;
                        for(auto i:parent.projections)
                        {
                            if(left.clip.Intersects(boxes[i])){left.projections.push_back(i);left.bounds.Add(Intersection(left.clip,boxes[i]));}
                            if(right.clip.Intersects(boxes[i])){right.projections.push_back(i);right.bounds.Add(Intersection(right.clip,boxes[i]));}
                        }
                        if(left.projections.empty()||right.projections.empty())continue;
                        const size_t total=references-parent.projections.size()+left.projections.size()+right.projections.size();
                        if(total>boxes.size()*2)continue;
                        const float cost=Area(left.bounds)*float(left.projections.size()+16)+Area(right.bounds)*float(right.projections.size()+16);
                        const float gain=parentCost-cost;
                        if(cost<parentCost*.75f&&gain>candidate.gain)
                        {candidate.gain=gain;candidate.extra=total-references;candidate.left=std::move(left);candidate.right=std::move(right);}
                    }
                }
            }
            if(candidate.gain>bestGain){bestGain=candidate.gain;best=g;}
        }
        if(best==groups.size())break;
        auto chosen=std::move(candidates[best]);
        references+=chosen.extra;
        groups[best]=std::move(chosen.left);groups.push_back(std::move(chosen.right));
        candidates[best]={};candidates.emplace_back();
    }
    return groups;
}
// A threaded, ordered BVH. Subtrees can be skipped without a shader stack, and
// visiting leaves preserves compositing order (including noncommutative blends).
struct CompositeNode
{
    Vec low;uint32_t next=0;
    Vec high;uint32_t leaf=std::numeric_limits<uint32_t>::max();
};
static_assert(sizeof(CompositeNode)==32);
template<class GetBounds>
uint32_t BuildCompositeNodes(std::vector<CompositeNode>& nodes,uint32_t first,uint32_t end,GetBounds get)
{
    const auto index=static_cast<uint32_t>(nodes.size());
    nodes.emplace_back();
    Bounds bounds;
    if(end-first==1){bounds=get(first);nodes[index].leaf=first;}
    else
    {
        const auto middle=first+(end-first)/2;
        const auto left=BuildCompositeNodes(nodes,first,middle,get);
        const auto right=BuildCompositeNodes(nodes,middle,end,get);
        bounds.Add(Bounds{nodes[left].low,nodes[left].high});
        bounds.Add(Bounds{nodes[right].low,nodes[right].high});
    }
    nodes[index].low=bounds.low;nodes[index].high=bounds.high;
    nodes[index].next=static_cast<uint32_t>(nodes.size());
    return index;
}
template<class Emit>
void VisitComposite(const std::vector<CompositeNode>& nodes,Vec point,Emit onLeaf)
{
    for(uint32_t i=0;i<nodes.size();)
    {
        const auto& n=nodes[i];
        if(!Bounds{n.low,n.high}.Contains(point)){i=n.next;continue;}
        if(n.leaf!=std::numeric_limits<uint32_t>::max()&&!onLeaf(n.leaf))return;
        ++i;
    }
}
}

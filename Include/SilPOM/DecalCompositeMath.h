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
};
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

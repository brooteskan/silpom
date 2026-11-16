// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>

namespace SilPOM
{
// Keep CPU eligibility within the bounded traversal in ReceiverUv/BakePrefix.azsli.
inline constexpr size_t ReceiverPaintMaxFixedDabs=32768;
inline constexpr size_t ReceiverPaintMaxNodes=65536;
inline bool ReceiverPaintPrefixFits(size_t fixed,size_t nodes=0)
{
    return fixed>0&&fixed<=ReceiverPaintMaxFixedDabs&&nodes<=ReceiverPaintMaxNodes;
}

// Visit all addresses, including anchors after legacy paint and on other layers.
template<class Layers,class Visitor>
void VisitPaintSurfaces(const Layers& layers,Visitor visit)
{
    for(const auto& layer:layers)
    {
        for(const auto& dab:layer.m_composite)visit(dab.m_point.m_surface);
        for(const auto& stroke:layer.m_strokes)
            for(const auto& point:stroke.m_points)visit(point.m_surface);
    }
}

// A missing receiver resolves to revision zero. Reappearance and any generation
// change must invalidate even if a particular anchor's position did not change.
template<class States,class Resolve>
bool RefreshPaintReceiverStates(States& states,Resolve resolve)
{
    bool changed=false;
    for(auto& [receiver,previous]:states)
    {
        const auto current=resolve(receiver);
        if(current!=previous){previous=current;changed=true;}
    }
    return changed;
}
}

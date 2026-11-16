// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/ReceiverPaintState.h"
#include <vector>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>

void Require(bool pass,const char* message)
{
    if(!pass){std::fprintf(stderr,"%s\n",message);std::exit(1);}
}
struct Point {int m_surface=0;};
struct Dab {Point m_point;};
struct Stroke {std::vector<Point> m_points;};
struct Layer {std::vector<Dab> m_composite;std::vector<Stroke> m_strokes;};
int main()
{
    using namespace SilPOM;
    std::vector<Layer> layers(2);
    // An unanchored composite must not hide later fixed or editable anchors.
    layers[0].m_composite={{{0}},{{1}}};
    layers[0].m_strokes={{{{2},{1}}}};
    layers[1].m_strokes={{{{3}}}};
    std::unordered_map<int,unsigned long long> live{{1,10},{2,20},{3,30}},tracked;
    VisitPaintSurfaces(layers,[&](int receiver){if(receiver)tracked.emplace(receiver,live[receiver]);});
    Require(tracked.size()==3,"Collect every referenced receiver after legacy paint and across layers");
    auto poll=[&]{return RefreshPaintReceiverStates(tracked,[&](int receiver){return live[receiver];});};
    Require(!poll(),"Unchanged receivers do not rebuild paint");
    ++live[2];
    Require(poll()&&!poll(),"Moving second receiver invalidates once while first stays unchanged");
    ++live[3];
    Require(poll(),"A new generation invalidates even when the first anchor remains fixed");
    live.erase(2);
    Require(poll(),"Missing receivers remove orphaned projections");
    live[2]=99;
    Require(poll(),"Reactivated receivers restore their projections");
    Require(!ReceiverPaintPrefixFits(0),"An empty prefix is ineligible");
    Require(ReceiverPaintPrefixFits(32768,65535),"Largest supported tree is eligible");
    Require(!ReceiverPaintPrefixFits(32769,65537),"Consolidated history beyond dab limit retains native replay");
    Require(!ReceiverPaintPrefixFits(1,65537),"Oversized node tree cannot publish an empty atlas");
    std::puts("Receiver paint state and prefix limits passed");
}

// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/SurfacePaintLayout.h"
#include <iostream>
#include <stdexcept>
using namespace SilPOM::SurfacePaintLayout;
void Require(bool pass,const char* text){if(!pass)throw std::runtime_error(text);}
int main()
{
    Usage combined;
    for(unsigned canvas=0;canvas<2;++canvas)for(unsigned chart=0;chart<48;++chart)
        Require(combined.Add(256)==Error::None,"Two 48-chart canvases must fit together");
    Require(combined.charts==96&&combined.bytes==268434432ull,"Receiver mip accounting changed");
    const auto before=combined;
    Require(combined.Add(256)==Error::Bytes,"The combined 97th chart must be rejected");
    Require(combined.charts==before.charts&&combined.bytes==before.bytes,"Rejected allocation changed accounting");
    // Recompute after erasing one chart, then permit its capacity to be reused.
    Usage reclaimed;
    for(unsigned chart=0;chart<95;++chart)Require(reclaimed.Add(256)==Error::None,"Reclaimed layout failed");
    Require(reclaimed.Add(256)==Error::None,"Erased chart capacity was not reusable");
    Usage small;
    for(unsigned chart=0;chart<MaxCharts;++chart)Require(small.Add(32)==Error::None,"Small chart capacity failed");
    Require(small.Add(32)==Error::Charts,"Receiver chart limit must be independent of byte limit");
    for(unsigned invalid:{0u,1u,31u,33u,1025u,0xffffffffu})
    {Usage layout;Require(layout.Add(invalid)==Error::Resolution&&layout.charts==0&&layout.bytes==0,"Invalid resolution was accepted");}
    Usage large;
    for(unsigned chart=0;chart<6;++chart)Require(large.Add(1024)==Error::None,"Valid mixed-resolution capacity failed");
    Require(large.Add(1024)==Error::Bytes,"Large mip layout exceeded receiver memory budget");
    std::cout<<"Receiver-wide chart/mip capacity, atomic rejection, and reclaimed capacity passed.\n";
}

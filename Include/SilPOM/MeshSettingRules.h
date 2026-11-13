// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace SilPOM::MeshSettingRules
{
// Editor policy only. Invalid externally authored runtime assets still fail
// validation. Assets, numeric quality budgets and decals are never rewritten.
struct Choices
{
    bool material=false,shared=false,proxy=false,proxyDepth=false;
    bool directional=false,proxyDirectional=false;
    bool bounds=false,neuralDepth=false,trained=false;
    std::uint32_t shadow=0,lighting=0;
    bool operator==(const Choices&) const = default;
};
inline Choices Normalize(Choices c)
{
    // Repair ambiguous older configurations deterministically: preserve
    // shared/comparison and proxy selections rather than the 4A-only flag.
    if(c.lighting!=0)c.shared=true;
    if(c.proxyDepth)c.proxy=true;
    if(c.shared||c.proxy)c.material=false;
    if(c.material||c.shared||c.proxy||c.directional||c.proxyDirectional||c.lighting!=0)c.shadow=3;
    if(c.trained)c.neuralDepth=true;
    if(c.neuralDepth)c.bounds=false;
    return c;
}
inline Choices Edit(const Choices& before,Choices after)
{
    // Newly edited choice wins. The Editor retains the previous accepted
    // choices, so switching either direction is unambiguous.
    if(after.material&&!before.material)
    {after.shared=false;after.proxy=false;after.proxyDepth=false;after.lighting=0;}
    if(after.shared&&!before.shared)after.material=false;
    if(after.proxy&&!before.proxy)after.material=false;
    if(after.proxyDepth&&!before.proxyDepth){after.proxy=true;after.material=false;}
    if(!after.proxy&&before.proxy)after.proxyDepth=false;
    if(after.lighting!=before.lighting&&after.lighting!=0){after.shared=true;after.material=false;}
    if(!after.shared&&before.shared)after.lighting=0;
    // Explicitly leaving neural-only disables its dependants; explicitly
    // enabling a dependant instead selects neural-only in Normalize().
    if(after.shadow!=before.shadow&&after.shadow!=3)
    {
        after.material=false;after.shared=false;after.proxy=false;after.proxyDepth=false;
        after.directional=false;after.proxyDirectional=false;after.lighting=0;
    }
    if(after.bounds&&!before.bounds&&!after.trained)after.neuralDepth=false;
    return Normalize(after);
}
}

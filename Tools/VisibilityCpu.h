// SPDX-License-Identifier: MIT
#pragma once
#include "LevelOneCpu.h"
namespace SilPOM::Cpu
{
inline thread_local std::array<uint,12> visibilityStats{};
inline thread_local std::array<float,10> visibilityFeatures{};
inline thread_local bool visibilityCaptured=false;
inline thread_local float visibilityProbability=.5f;
inline thread_local const float* visibilityOverride=nullptr;
inline bool OverrideVisibility(float* feature,float& value)
{
    if(!visibilityOverride) return false;
    value=visibilityOverride[144];
    for(uint j=0;j<12;++j)
    {
        float unit=visibilityOverride[120+j];
        for(uint k=0;k<10;++k) unit+=feature[k]*visibilityOverride[j*10+k];
        unit=max(unit,0.f);value+=unit*unit*visibilityOverride[132+j];
    }
    return true;
}
#define SILPOM_CPU 1
#define SP_TEX const Texture&
#define SP_REF(T) T&
#define SP_UNROLL
#define SP_BRANCH
#define SILPOM_SHADOW_STAT(event) ++visibilityStats[event]
#define SILPOM_CAPTURE_VISIBILITY(feature,ray) std::copy(feature,feature+10,visibilityFeatures.begin());visibilityCaptured=true
#define SILPOM_VISIBILITY_PROBABILITY(value) visibilityProbability=value
#define SILPOM_OVERRIDE_VISIBILITY(feature,value) OverrideVisibility(feature,value)
#include "../Assets/Shaders/SilPOM/ReliefShadow.azsli"
#include "../Assets/Shaders/SilPOM/LevelThree.azsli"
#undef SILPOM_OVERRIDE_VISIBILITY
#undef SILPOM_VISIBILITY_PROBABILITY
#undef SILPOM_CAPTURE_VISIBILITY
#undef SILPOM_SHADOW_STAT
#undef SP_BRANCH
#undef SP_UNROLL
#undef SP_REF
#undef SP_TEX
#undef SILPOM_CPU
}

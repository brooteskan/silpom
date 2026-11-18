// SPDX-License-Identifier: MIT
// Prints the float32 bits of the reference networks in Tools/CompiledNetworks
// (formerly compiled into the shaders), as the C++ compiler parses them. ProfileWeightsTests.py compares them
// with Tools/silpom_profile.py, whose weights files feed the profile assets.
#include <cstdint>
#include <cstdio>
#include <cstring>
struct float2 { float x = 0, y = 0; float2() = default; constexpr float2(float a, float b) : x(a), y(b) {} };
struct float4 { float x, y, z, w; constexpr float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {} };
struct uint2 { std::uint32_t x, y; constexpr uint2(std::uint32_t a, std::uint32_t b) : x(a), y(b) {} };
#include "../Tools/CompiledNetworks/TowerNoMarchWeights.azsli"
#include "../Tools/CompiledNetworks/NeuralTowerWeights.azsli"
#include "../Tools/CompiledNetworks/NeuralTowerCacheLayout.azsli"
#include "../Tools/CompiledNetworks/DirectionalEstimatorWeights.azsli"
#include "../Tools/CompiledNetworks/AngularEdgeWeights.azsli"
#include "../Tools/CompiledNetworks/NeuralWeldedTowerWeights.azsli"
#include "../Tools/CompiledNetworks/NeuralOpenShaftTowerWeights.azsli"
#include "../Tools/CompiledNetworks/NeuralTowerLevelTwoWeights.azsli"
#include "../Tools/CompiledNetworks/NeuralVisibilityWeights.azsli"
#include "../Tools/CompiledNetworks/NeuralVisibilityPhysicalWeights.azsli"
#include "../Tools/CompiledNetworks/VisibilityClassificationThresholds.azsli"

static void Dump(const char* name, const void* data, size_t bytes)
{
    const auto* values = static_cast<const float*>(data);
    for (size_t i = 0; i < bytes / sizeof(float); ++i)
    {
        std::uint32_t bits;
        std::memcpy(&bits, values + i, 4);
        std::printf("%s %zu %08x\n", name, i, bits);
    }
}
#define DUMP(array) Dump(#array, &array, sizeof(array))
#define HIT(p) DUMP(p##W0); DUMP(p##B0); DUMP(p##W1); DUMP(p##B1)
#define L2(p) DUMP(L2##p##W0); DUMP(L2##p##W1); DUMP(L2##p##B0); DUMP(L2##p##B1)
#define VIS(p) DUMP(Visibility##p##W0); DUMP(Visibility##p##B0); DUMP(Visibility##p##W1); DUMP(Visibility##p##B1); \
    DUMP(PhysicalVisibility##p##W0); DUMP(PhysicalVisibility##p##B0); DUMP(PhysicalVisibility##p##W1); \
    DUMP(PhysicalVisibility##p##B1); DUMP(Visibility##p##Threshold)

int main()
{
    static_assert(sizeof(float2) == 8 && sizeof(float4) == 16 && sizeof(uint2) == 8, "vectors must be packed");
    HIT(TowerNoMarch);
    DUMP(TowerNeuralHeightWeights); DUMP(TowerNeuralSlopeWeights); DUMP(TowerNeuralBias); DUMP(TowerNeuralOutput);
    DUMP(TowerNeuralOutputBias); DUMP(TowerNeuralTileRows);
    HIT(WeldedWall); HIT(WeldedStairs); HIT(WeldedDoor);
    HIT(OpenShaftWall); HIT(OpenShaftStairs); HIT(OpenShaftTrim); HIT(OpenShaftReveal);
    L2(Wall); L2(Stairs); L2(Trim); L2(Reveal);
    VIS(Wall); VIS(Stairs); VIS(Trim); VIS(Reveal);
    HIT(SilPomDirectionalField); HIT(SilPomDirectionalCaps4); HIT(SilPomDirectionalCaps8); HIT(SilPomAngularEdges);
    return 0;
}

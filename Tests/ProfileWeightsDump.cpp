// SPDX-License-Identifier: MIT
// Prints the float32 bits of every compiled-in trained network, as the C++
// compiler parsed the shader headers. ProfileWeightsTests.py compares them
// with Tools/silpom_profile.py, whose weights files feed the profile assets.
#include <cstdint>
#include <cstdio>
#include <cstring>
struct float2 { float x = 0, y = 0; float2() = default; constexpr float2(float a, float b) : x(a), y(b) {} };
#include "../Assets/Shaders/SilPOM/NeuralWeldedTowerWeights.azsli"
#include "../Assets/Shaders/SilPOM/NeuralOpenShaftTowerWeights.azsli"
#include "../Assets/Shaders/SilPOM/NeuralTowerLevelTwoWeights.azsli"
#include "../Assets/Shaders/SilPOM/NeuralVisibilityWeights.azsli"
#include "../Assets/Shaders/SilPOM/NeuralVisibilityPhysicalWeights.azsli"
#include "../Assets/Shaders/SilPOM/VisibilityClassificationThresholds.azsli"

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
    static_assert(sizeof(float2) == 8, "float2 must be two packed floats");
    HIT(WeldedWall); HIT(WeldedStairs); HIT(WeldedDoor);
    HIT(OpenShaftWall); HIT(OpenShaftStairs); HIT(OpenShaftTrim); HIT(OpenShaftReveal);
    L2(Wall); L2(Stairs); L2(Trim); L2(Reveal);
    VIS(Wall); VIS(Stairs); VIS(Trim); VIS(Reveal);
    return 0;
}

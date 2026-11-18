// SPDX-License-Identifier: MIT
#pragma once
// CPU reference: the networks that were compiled into the shaders before they
// became surface profile data, packed in the profile weights layout
// (ProfileNetworks.azsli). Include inside the CPU shader namespace, after
// ProfileNetworks.azsli. Shader model ids 0-7; roles and flags as the tower
// profiles declare them. Model 0 is the seed network, its cache layout and the
// no-march hit network.
#include <array>
#include <cstring>
#include "TowerNoMarchWeights.azsli"
#include "NeuralTowerWeights.azsli"
#include "NeuralWeldedTowerWeights.azsli"
#include "NeuralOpenShaftTowerWeights.azsli"
#include "NeuralTowerLevelTwoWeights.azsli"
#include "NeuralVisibilityWeights.azsli"
#include "NeuralVisibilityPhysicalWeights.azsli"
#include "VisibilityClassificationThresholds.azsli"

namespace CompiledNetworks
{
struct uint2 { uint x, y; constexpr uint2(uint a, uint b) : x(a), y(b) {} };
#include "NeuralTowerCacheLayout.azsli"
using Packed = std::array<float, SP_NET_FLOATS>;
template<class T> inline void Put(Packed& out, uint offset, const T& array) { std::memcpy(out.data() + offset, &array, sizeof(array)); }
#define SILPOM_PACK_HIT(out, p) Put(out, 0, p##W0); Put(out, 176, p##B0); Put(out, 192, p##W1); Put(out, 224, p##B1)
#define SILPOM_PACK_OPEN(out, p) \
    Put(out, SP_NET_LEVEL_TWO, L2##p##W0); Put(out, SP_NET_LEVEL_TWO + 456, L2##p##B0); \
    Put(out, SP_NET_LEVEL_TWO + 480, L2##p##W1); Put(out, SP_NET_LEVEL_TWO + 600, L2##p##B1); \
    Put(out, SP_NET_VISIBILITY, Visibility##p##W0); Put(out, SP_NET_VISIBILITY + 120, Visibility##p##B0); \
    Put(out, SP_NET_VISIBILITY + 132, Visibility##p##W1); Put(out, SP_NET_VISIBILITY + 144, Visibility##p##B1); \
    Put(out, SP_NET_PHYSICAL_VISIBILITY, PhysicalVisibility##p##W0); Put(out, SP_NET_PHYSICAL_VISIBILITY + 120, PhysicalVisibility##p##B0); \
    Put(out, SP_NET_PHYSICAL_VISIBILITY + 132, PhysicalVisibility##p##W1); Put(out, SP_NET_PHYSICAL_VISIBILITY + 144, PhysicalVisibility##p##B1); \
    Put(out, SP_NET_PHYSICAL_THRESHOLD, Visibility##p##Threshold)
inline const Packed& Weights(uint id)
{
    static const std::array<Packed, 8> table = []
    {
        static_assert(sizeof(float2) == 8, "float2 must be two packed floats");
        std::array<Packed, 8> t{};
        SILPOM_PACK_HIT(t[0], TowerNoMarch);
        for (uint i = 0; i < 8; ++i)
        {
            const auto& h = TowerNeuralHeightWeights[i];
            t[0][SP_NET_SEED + i * 4] = h.x; t[0][SP_NET_SEED + i * 4 + 1] = h.y;
            t[0][SP_NET_SEED + i * 4 + 2] = h.z; t[0][SP_NET_SEED + i * 4 + 3] = h.w;
            t[0][SP_NET_SEED + 32 + i * 2] = TowerNeuralSlopeWeights[i].x;
            t[0][SP_NET_SEED + 33 + i * 2] = TowerNeuralSlopeWeights[i].y;
        }
        Put(t[0], SP_NET_SEED + 48, TowerNeuralBias); Put(t[0], SP_NET_SEED + 56, TowerNeuralOutput);
        t[0][SP_NET_SEED + 64] = TowerNeuralOutputBias;
        const uint tiles[2] = {13, 12};
        Put(t[0], SP_NET_CACHE_LAYOUT, tiles); Put(t[0], SP_NET_CACHE_LAYOUT + 2, TowerNeuralTileRows);
        SILPOM_PACK_HIT(t[1], WeldedWall); SILPOM_PACK_HIT(t[2], WeldedStairs); SILPOM_PACK_HIT(t[3], WeldedDoor);
        SILPOM_PACK_HIT(t[4], OpenShaftWall); SILPOM_PACK_OPEN(t[4], Wall);
        SILPOM_PACK_HIT(t[5], OpenShaftStairs); SILPOM_PACK_OPEN(t[5], Stairs);
        SILPOM_PACK_HIT(t[6], OpenShaftTrim); SILPOM_PACK_OPEN(t[6], Trim);
        SILPOM_PACK_HIT(t[7], OpenShaftReveal); SILPOM_PACK_OPEN(t[7], Reveal);
        return t;
    }();
    return table[id < 8 ? id : 0];
}
#undef SILPOM_PACK_OPEN
#undef SILPOM_PACK_HIT
} // namespace CompiledNetworks

// The compiled network set of shader model id (0-7) as a trained model.
inline SilPomTrainedModel SilPomCompiledModel(uint id)
{
    SilPomTrainedModel model{};
    model.id = id;
    model.role = (id == 1u || id == 4u) ? SP_ROLE_WALL : (id == 6u || id == 7u) ? SP_ROLE_OPENING : SP_ROLE_SURFACE;
    model.flags = (id >= 4u && id <= 7u) ? (SP_PROFILE_LEVEL_TWO | SP_PROFILE_VISIBILITY)
        : id == 0u ? (SP_PROFILE_SEED | SP_PROFILE_CACHE_LAYOUT) : 0u;
    model.weights.data = CompiledNetworks::Weights(id).data();
    return model;
}

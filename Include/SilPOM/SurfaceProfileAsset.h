// SPDX-License-Identifier: MIT
#pragma once
#include <AzCore/Asset/AssetCommon.h>
#include <AzCore/RTTI/TypeInfoSimple.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/string/string.h>

namespace SilPOM
{
// How a trained profile's surface is used by coverage and backup shells.
enum class SurfaceRole : AZ::u32 { Surface = 0, Wall = 1, Opening = 2 };
AZ_TYPE_INFO_SPECIALIZE(SurfaceRole, "{6A1D5B52-8F8D-4C3B-A0E2-55C0D1E8B7A4}");

// One trained displacement profile: the height image and relief parameters it
// was trained for, plus its networks. Built from a .silpomprofile source and
// its .silpomweights.json (see Tools/silpom_profile.py).
class SurfaceProfileAsset final : public AZ::Data::AssetData
{
public:
    AZ_RTTI(SurfaceProfileAsset, "{0C6E35A3-4C4F-4E0C-9E39-2E5C3A7F1D61}", AZ::Data::AssetData);
    AZ_CLASS_ALLOCATOR(SurfaceProfileAsset, AZ::SystemAllocator);
    static void Reflect(AZ::ReflectContext* context);

    // Flattened network sizes; each network is stored W0, B0, W1, B1.
    static constexpr size_t HitModelFloats = 16 * 11 + 16 + 16 * 2 + 2;
    static constexpr size_t LevelTwoFloats = 24 * 19 + 24 + 24 * 5 + 5;
    static constexpr size_t VisibilityFloats = 12 * 10 + 12 + 12 + 1;
    // Model 0's seed network: 8x4 height and 8x2 slope weights, 8 biases,
    // 8 outputs and an output bias.
    static constexpr size_t SeedFloats = 8 * 4 + 8 * 2 + 8 + 8 + 1;
    // Seed cache layout: tiles x, y; per tile row two words of ten 3-bit experts.
    static constexpr size_t MaxCacheRows = 12, MaxCacheColumns = 20, CacheTile = 512;
    static constexpr size_t CacheLayoutWords = 2 + MaxCacheRows * 2;
    // Offsets of the GPU weights buffer (SP_NET_* in ProfileNetworks.azsli).
    static constexpr size_t LevelTwoOffset = HitModelFloats;
    static constexpr size_t VisibilityOffset = LevelTwoOffset + LevelTwoFloats;
    static constexpr size_t PhysicalVisibilityOffset = VisibilityOffset + VisibilityFloats;
    static constexpr size_t PhysicalThresholdOffset = PhysicalVisibilityOffset + VisibilityFloats;
    static constexpr size_t SeedOffset = PhysicalThresholdOffset + 1;
    static constexpr size_t CacheLayoutOffset = SeedOffset + SeedFloats;
    static constexpr size_t NetworkFloats = CacheLayoutOffset + CacheLayoutWords;
    // Lighting estimators (LightingEstimator 2-5), after the training image
    // size; each W0 32x20, B0 32, W1 outputs x 32, B1.
    // (SP_NET_ESTIMATORS etc. in ProfileNetworks.azsli.)
    static constexpr size_t EstimatorCount = 4;
    static constexpr size_t EstimatorOffset = NetworkFloats;
    static constexpr size_t EstimatorOutputs(size_t i) { return i == 0 ? 128 : i == 1 ? 28 : i == 2 ? 56 : 32; }
    static constexpr size_t EstimatorFloats(size_t i) { return 32 * 20 + 32 + EstimatorOutputs(i) * 33; }
    static constexpr size_t EstimatorNetworkOffset(size_t i)
    {
        size_t offset = NetworkFloats + 2;
        for (size_t k = 0; k < i; ++k) offset += 32 * 20 + 32 + EstimatorOutputs(k) * 33;
        return offset;
    }
    static constexpr size_t EstimatorBufferFloats = 11955;
    static constexpr const char* EstimatorName(size_t i)
    { return i == 0 ? "DirectionalField32" : i == 1 ? "AngularOccluders4" : i == 2 ? "AngularOccluders8" : "AngularEdges4"; }
    // Flags of the networks present beyond the hit model (SP_PROFILE_*).
    static constexpr AZ::u32 LevelTwoFlag = 1, VisibilityFlag = 2, SeedFlag = 4, CacheLayoutFlag = 8;
    // Estimator i (LightingEstimator i + 2) present.
    static constexpr AZ::u32 EstimatorFlag(size_t i) { return 16u << i; }
    bool HasEstimator(size_t i) const { return i < EstimatorCount && !Estimator(i).empty(); }
    const AZStd::vector<float>& Estimator(size_t i) const
    { return i == 0 ? m_directionalField : i == 1 ? m_angularOccluders4 : i == 2 ? m_angularOccluders8 : m_angularEdges4; }
    AZStd::vector<float>& Estimator(size_t i)
    { return i == 0 ? m_directionalField : i == 1 ? m_angularOccluders4 : i == 2 ? m_angularOccluders8 : m_angularEdges4; }

    bool HasOpenShaftNetworks() const { return !m_levelTwo.empty(); }
    bool HasSeed() const { return !m_seed.empty(); }
    bool HasCacheLayout() const { return !m_cacheLayout.empty(); }
    AZ::u32 NetworkFlags() const
    {
        return (HasOpenShaftNetworks() ? LevelTwoFlag | VisibilityFlag : 0u) | (HasSeed() ? SeedFlag : 0u)
            | (HasCacheLayout() ? CacheLayoutFlag : 0u)
            | (HasEstimator(0) ? EstimatorFlag(0) : 0u) | (HasEstimator(1) ? EstimatorFlag(1) : 0u)
            | (HasEstimator(2) ? EstimatorFlag(2) : 0u) | (HasEstimator(3) ? EstimatorFlag(3) : 0u);
    }
    // All networks in the GPU weights buffer layout; absent ones are zero. The
    // buffer extends to the estimators only if the profile has any.
    AZStd::vector<float> PackNetworks() const;
    // Sizes, ranges and finiteness; an empty string when valid.
    AZStd::string Validate() const;

    // Product path of the training height image. Model 0 profiles may omit it:
    // they then apply to each binding's own height image of this size.
    AZStd::string m_heightProduct;
    AZ::u32 m_width = 0, m_heightPixels = 0;
    float m_scale = 0, m_reference = 0;
    AZ::u32 m_addressMode = 0;
    SurfaceRole m_role = SurfaceRole::Surface;
    // 0: model 0 (seed network, optional seed cache, no-march hit network);
    // 1-3 welded, 4-7 open-shaft profiles.
    AZ::u32 m_shaderModel = 0;
    AZStd::vector<float> m_hitModel, m_levelTwo, m_visibility, m_physicalVisibility, m_seed;
    float m_physicalVisibilityThreshold = 0;
    AZStd::vector<AZ::u32> m_cacheLayout;  // model 0 with a seed cache
    AZStd::string m_cacheProduct;          // product path of the seed cache image, optional
    // Optional lighting estimator heads (LightingEstimator 2-5).
    AZStd::vector<float> m_directionalField, m_angularOccluders4, m_angularOccluders8, m_angularEdges4;
    // The profile's "Networks" folder of trained networks (relative to the
    // profile); empty when its weights are the Tools/CompiledNetworks reference.
    AZStd::string m_networks;
    AZStd::string m_generation;     // SHA-256 of the profile and weights sources
};
} // namespace SilPOM

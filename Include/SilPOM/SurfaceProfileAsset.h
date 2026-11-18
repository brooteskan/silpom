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
    // Offsets of the GPU weights buffer (SP_NET_* in ProfileNetworks.azsli).
    static constexpr size_t LevelTwoOffset = HitModelFloats;
    static constexpr size_t VisibilityOffset = LevelTwoOffset + LevelTwoFloats;
    static constexpr size_t PhysicalVisibilityOffset = VisibilityOffset + VisibilityFloats;
    static constexpr size_t PhysicalThresholdOffset = PhysicalVisibilityOffset + VisibilityFloats;
    static constexpr size_t NetworkFloats = PhysicalThresholdOffset + 1;
    // Flags of the networks present beyond the hit model (SP_PROFILE_*).
    static constexpr AZ::u32 LevelTwoFlag = 1, VisibilityFlag = 2;

    bool HasOpenShaftNetworks() const { return !m_levelTwo.empty(); }
    AZ::u32 NetworkFlags() const { return HasOpenShaftNetworks() ? LevelTwoFlag | VisibilityFlag : 0u; }
    // All networks in the GPU weights buffer layout; absent ones are zero.
    AZStd::vector<float> PackNetworks() const;
    // Sizes, ranges and finiteness; an empty string when valid.
    AZStd::string Validate() const;

    AZStd::string m_heightProduct;  // product path of the training height image
    AZ::u32 m_width = 0, m_heightPixels = 0;
    float m_scale = 0, m_reference = 0;
    AZ::u32 m_addressMode = 0;
    SurfaceRole m_role = SurfaceRole::Surface;
    // Selects the compiled-in shader networks until they are read from the
    // asset; m_hitModel etc. are bit-identical copies of those networks.
    AZ::u32 m_shaderModel = 0;
    AZStd::vector<float> m_hitModel, m_levelTwo, m_visibility, m_physicalVisibility;
    float m_physicalVisibilityThreshold = 0;
    AZStd::string m_generation;     // SHA-256 of the profile and weights sources
};
} // namespace SilPOM

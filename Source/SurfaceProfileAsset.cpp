// SPDX-License-Identifier: MIT
#include <SilPOM/SurfaceProfileAsset.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/std/algorithm.h>
#include <cmath>
#include <cstring>

namespace SilPOM
{
void SurfaceProfileAsset::Reflect(AZ::ReflectContext* context)
{
    if (auto* sc = azrtti_cast<AZ::SerializeContext*>(context))
    {
        sc->Enum<SurfaceRole>()->Value("Surface", SurfaceRole::Surface)->Value("Wall", SurfaceRole::Wall)
            ->Value("Opening", SurfaceRole::Opening);
        sc->Class<SurfaceProfileAsset, AZ::Data::AssetData>()->Version(5) // networks provenance
            ->Field("HeightProduct", &SurfaceProfileAsset::m_heightProduct)
            ->Field("Width", &SurfaceProfileAsset::m_width)->Field("HeightPixels", &SurfaceProfileAsset::m_heightPixels)
            ->Field("Scale", &SurfaceProfileAsset::m_scale)->Field("Reference", &SurfaceProfileAsset::m_reference)
            ->Field("AddressMode", &SurfaceProfileAsset::m_addressMode)->Field("Role", &SurfaceProfileAsset::m_role)
            ->Field("ShaderModel", &SurfaceProfileAsset::m_shaderModel)
            ->Field("HitModel", &SurfaceProfileAsset::m_hitModel)->Field("LevelTwo", &SurfaceProfileAsset::m_levelTwo)
            ->Field("Visibility", &SurfaceProfileAsset::m_visibility)
            ->Field("PhysicalVisibility", &SurfaceProfileAsset::m_physicalVisibility)
            ->Field("PhysicalVisibilityThreshold", &SurfaceProfileAsset::m_physicalVisibilityThreshold)
            ->Field("Seed", &SurfaceProfileAsset::m_seed)->Field("CacheLayout", &SurfaceProfileAsset::m_cacheLayout)
            ->Field("CacheProduct", &SurfaceProfileAsset::m_cacheProduct)
            ->Field("DirectionalField32", &SurfaceProfileAsset::m_directionalField)
            ->Field("AngularOccluders4", &SurfaceProfileAsset::m_angularOccluders4)
            ->Field("AngularOccluders8", &SurfaceProfileAsset::m_angularOccluders8)
            ->Field("AngularEdges4", &SurfaceProfileAsset::m_angularEdges4)
            ->Field("Networks", &SurfaceProfileAsset::m_networks)
            ->Field("Generation", &SurfaceProfileAsset::m_generation);
    }
}

static_assert(SurfaceProfileAsset::EstimatorNetworkOffset(1) == 6111 && SurfaceProfileAsset::EstimatorNetworkOffset(3) == 10227
    && SurfaceProfileAsset::EstimatorNetworkOffset(SurfaceProfileAsset::EstimatorCount) == SurfaceProfileAsset::EstimatorBufferFloats,
    "estimator offsets must match ProfileNetworks.azsli");

AZStd::vector<float> SurfaceProfileAsset::PackNetworks() const
{
    const bool estimators = HasEstimator(0) || HasEstimator(1) || HasEstimator(2) || HasEstimator(3);
    AZStd::vector<float> packed(estimators ? EstimatorBufferFloats : NetworkFloats, 0.0f);
    AZStd::copy(m_hitModel.begin(), m_hitModel.end(), packed.begin());
    if (HasOpenShaftNetworks())
    {
        AZStd::copy(m_levelTwo.begin(), m_levelTwo.end(), packed.begin() + LevelTwoOffset);
        AZStd::copy(m_visibility.begin(), m_visibility.end(), packed.begin() + VisibilityOffset);
        AZStd::copy(m_physicalVisibility.begin(), m_physicalVisibility.end(), packed.begin() + PhysicalVisibilityOffset);
        packed[PhysicalThresholdOffset] = m_physicalVisibilityThreshold;
    }
    AZStd::copy(m_seed.begin(), m_seed.end(), packed.begin() + SeedOffset);
    // Layout words are uints; the shader reads their bits.
    if (HasCacheLayout()) std::memcpy(packed.data() + CacheLayoutOffset, m_cacheLayout.data(), m_cacheLayout.size() * 4);
    if (estimators)
    {
        // Footprints are measured in texels of the training image.
        packed[EstimatorOffset] = float(m_width);
        packed[EstimatorOffset + 1] = float(m_heightPixels);
        for (size_t i = 0; i < EstimatorCount; ++i)
            AZStd::copy(Estimator(i).begin(), Estimator(i).end(), packed.begin() + EstimatorNetworkOffset(i));
    }
    return packed;
}
AZStd::string SurfaceProfileAsset::Validate() const
{
    const auto finite = [](const AZStd::vector<float>& values)
    { return AZStd::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); }); };
    if ((m_heightProduct.empty() && m_shaderModel != 0) || !m_width || !m_heightPixels) return "missing height image or size";
    if (!std::isfinite(m_scale) || !std::isfinite(m_reference) || m_reference < 0 || m_reference > 1) return "invalid scale or reference";
    if (m_addressMode > 1 || static_cast<AZ::u32>(m_role) > 2) return "invalid address mode or role";
    if (m_shaderModel > 7) return "shader model must be 0-7";
    if (m_hitModel.size() != HitModelFloats || !finite(m_hitModel)) return "hit model must have 226 finite weights";
    // Level 2 and both visibility networks come together, for any role and model.
    const bool openShaft = HasOpenShaftNetworks();
    if (openShaft != !m_visibility.empty() || openShaft != !m_physicalVisibility.empty())
        return "Level 2, visibility and physical visibility networks must be present together";
    if (openShaft && (m_levelTwo.size() != LevelTwoFloats || m_visibility.size() != VisibilityFloats
        || m_physicalVisibility.size() != VisibilityFloats || !finite(m_levelTwo) || !finite(m_visibility)
        || !finite(m_physicalVisibility) || !std::isfinite(m_physicalVisibilityThreshold)))
        return "invalid Level 2 or visibility networks";
    // Model 0 carries its seed network; a seed cache needs its layout.
    if ((m_shaderModel == 0) != HasSeed()) return "model 0 profiles (and only they) need a seed network";
    if (HasSeed() && (m_seed.size() != SeedFloats || !finite(m_seed))) return "seed network must have 65 finite weights";
    if (!m_cacheProduct.empty() && !HasCacheLayout()) return "a seed cache needs its CacheLayout";
    for (size_t i = 0; i < EstimatorCount; ++i)
        if (HasEstimator(i) && (Estimator(i).size() != EstimatorFloats(i) || !finite(Estimator(i))))
            return AZStd::string::format("%s must have %zu finite weights", EstimatorName(i), EstimatorFloats(i));
    if (HasCacheLayout())
    {
        if (!HasSeed() || m_cacheLayout.size() != CacheLayoutWords) return "invalid seed cache layout";
        const AZ::u32 columns = m_cacheLayout[0], rows = m_cacheLayout[1];
        if (columns == 0 || rows == 0 || columns > MaxCacheColumns || rows > MaxCacheRows
            || columns * CacheTile != m_width || rows * CacheTile != m_heightPixels)
            return "seed cache tiles must cover the height image in 512-texel tiles (at most 20 x 12)";
    }
    return {};
}
} // namespace SilPOM

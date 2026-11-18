// SPDX-License-Identifier: MIT
#include <SilPOM/SurfaceProfileAsset.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/std/algorithm.h>
#include <cmath>

namespace SilPOM
{
void SurfaceProfileAsset::Reflect(AZ::ReflectContext* context)
{
    if (auto* sc = azrtti_cast<AZ::SerializeContext*>(context))
    {
        sc->Enum<SurfaceRole>()->Value("Surface", SurfaceRole::Surface)->Value("Wall", SurfaceRole::Wall)
            ->Value("Opening", SurfaceRole::Opening);
        sc->Class<SurfaceProfileAsset, AZ::Data::AssetData>()->Version(1)
            ->Field("HeightProduct", &SurfaceProfileAsset::m_heightProduct)
            ->Field("Width", &SurfaceProfileAsset::m_width)->Field("HeightPixels", &SurfaceProfileAsset::m_heightPixels)
            ->Field("Scale", &SurfaceProfileAsset::m_scale)->Field("Reference", &SurfaceProfileAsset::m_reference)
            ->Field("AddressMode", &SurfaceProfileAsset::m_addressMode)->Field("Role", &SurfaceProfileAsset::m_role)
            ->Field("ShaderModel", &SurfaceProfileAsset::m_shaderModel)
            ->Field("HitModel", &SurfaceProfileAsset::m_hitModel)->Field("LevelTwo", &SurfaceProfileAsset::m_levelTwo)
            ->Field("Visibility", &SurfaceProfileAsset::m_visibility)
            ->Field("PhysicalVisibility", &SurfaceProfileAsset::m_physicalVisibility)
            ->Field("PhysicalVisibilityThreshold", &SurfaceProfileAsset::m_physicalVisibilityThreshold)
            ->Field("Generation", &SurfaceProfileAsset::m_generation);
    }
}

AZStd::string SurfaceProfileAsset::Validate() const
{
    const auto finite = [](const AZStd::vector<float>& values)
    { return AZStd::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); }); };
    if (m_heightProduct.empty() || !m_width || !m_heightPixels) return "missing height image or size";
    if (!std::isfinite(m_scale) || !std::isfinite(m_reference) || m_reference < 0 || m_reference > 1) return "invalid scale or reference";
    if (m_addressMode > 1 || static_cast<AZ::u32>(m_role) > 2) return "invalid address mode or role";
    if (m_shaderModel < 1 || m_shaderModel > 7) return "shader model must be 1-7";
    if (m_hitModel.size() != HitModelFloats || !finite(m_hitModel)) return "hit model must have 226 finite weights";
    // Models 4-7 also carry Level 2 and both visibility networks.
    const bool openShaft = m_shaderModel >= 4;
    if (openShaft != HasOpenShaftNetworks()) return "open-shaft profiles (models 4-7) need Level 2 and visibility networks";
    if (openShaft && (m_levelTwo.size() != LevelTwoFloats || m_visibility.size() != VisibilityFloats
        || m_physicalVisibility.size() != VisibilityFloats || !finite(m_levelTwo) || !finite(m_visibility)
        || !finite(m_physicalVisibility) || !std::isfinite(m_physicalVisibilityThreshold)))
        return "invalid Level 2 or visibility networks";
    if (!openShaft && (!m_visibility.empty() || !m_physicalVisibility.empty())) return "unexpected visibility networks";
    return {};
}
} // namespace SilPOM

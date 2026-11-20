// SPDX-License-Identifier: MIT
#pragma once
#include <string_view>

namespace SilPOM::StrictNeuralPolicy
{
// The inspector's directional/angular comparisons are wall-model controls.
// Other supported profiles retain their physical neural-only visibility route.
inline unsigned LightingEstimatorForProfile(unsigned model,unsigned requested)
{ return model==4u?requested:0u; }
// ShadowVisibility: the SilPOMHitShadow pass's shader. It evaluates the same
// neural-only relief visibility as the forward shader (one compile boundary),
// but it is not the batch's lighting proof.
enum class Capability { Unsupported, Flat, Visibility, ShadowVisibility, TrainedDepth, AnalyticDepth };

// Tags alone are insufficient: a material can override a shader while keeping
// its logical tag. Certify the actual built-in generated product as well.
inline Capability Classify(std::string_view tag, std::string_view product, std::string_view supervariant)
{
    if (!supervariant.empty() && supervariant != "NoMSAA") return Capability::Unsupported;
    constexpr std::string_view prefixes[] = {
        "materials/types/silpommesh/silpommesh_mainpipeline_",
        "materials/types/silpommesh/silpommeshuv_mainpipeline_"};
    bool generated = false;
    for (const auto prefix : prefixes)
        if (product.substr(0, prefix.size()) == prefix)
        { product.remove_prefix(prefix.size()); generated = true; break; }
    if (!generated) return Capability::Unsupported;
    struct Known { std::string_view tag, product; Capability capability; };
    constexpr Known known[] = {
        {"shadow", "shadowmappass.azshader", Capability::Flat},
        {"shadow_customZ", "shadowmappass_customz.azshader", Capability::Flat},
        {"silpom_uv_bake", "silpomuvbake.azshader", Capability::Flat},
        {"silpom_uv_bake_shared", "silpomuvbakeshared.azshader", Capability::Flat},
        {"silpom_uv_bake_learned", "silpomuvbakelearned.azshader", Capability::Flat},
        {"silpom_uv_usage", "silpomuvusage.azshader", Capability::Flat},
        {"silpom_hit_forward_neural_only", "silpomhitforwardneuralonly.azshader", Capability::Visibility},
        {"silpom_hit_shadow", "silpomhitshadow.azshader", Capability::ShadowVisibility},
        {"silpom_hit_forward_learned_material", "silpomhitforwardlearnedmaterial.azshader", Capability::Visibility},
        {"silpom_hit_forward_level_five", "silpomhitforwardlevelfive.azshader", Capability::Visibility},
        {"silpom_hit_forward_shared", "silpomhitforwardshared.azshader", Capability::Visibility},
        {"silpom_hit_forward_shared_proxy", "silpomhitforwardsharedproxy.azshader", Capability::Visibility},
        {"silpom_hit_forward_shared_compare", "silpomhitforwardsharedcompare.azshader", Capability::Visibility},
        {"silpom_hit_forward_shared_proxy_compare", "silpomhitforwardsharedproxycompare.azshader", Capability::Visibility},
        {"silpom_hit_forward_shared_ambient", "silpomhitforwardsharedambient.azshader", Capability::Visibility},
        {"silpom_hit_forward_shared_proxy_ambient", "silpomhitforwardsharedproxyambient.azshader", Capability::Visibility},
        {"silpom_hit_forward_angular", "silpomhitforwardangular.azshader", Capability::Visibility},
        {"silpom_hit_forward_angular_proxy", "silpomhitforwardangularproxy.azshader", Capability::Visibility},
        {"silpom_hit_forward_angular_ambient", "silpomhitforwardangularambient.azshader", Capability::Visibility},
        {"silpom_hit_forward_angular_proxy_ambient", "silpomhitforwardangularproxyambient.azshader", Capability::Visibility},
        {"silpom_hit_depth_neural_cache", "silpomhitdepthneuralcache.azshader", Capability::TrainedDepth},
        {"silpom_hit_depth_lod_near", "silpomhitdepthlodnear.azshader", Capability::TrainedDepth},
        {"silpom_hit_depth_lod_near_wall", "silpomhitdepthlodnearwall.azshader", Capability::TrainedDepth},
        {"silpom_hit_depth_lod_shell", "silpomhitdepthlodshell.azshader", Capability::AnalyticDepth},
        {"silpom_hit_depth_wall_backup", "silpomhitdepthwallbackup.azshader", Capability::AnalyticDepth},
        {"silpom_hit_depth_prepass", "silpomhitdepthprepass.azshader", Capability::AnalyticDepth},
        {"silpom_hit_depth_proxy", "silpomhitdepthproxy.azshader", Capability::AnalyticDepth},
        {"silpom_hit_depth_learned_proxy", "silpomhitdepthlearnedproxy.azshader", Capability::AnalyticDepth}
    };
    for (const auto& entry : known)
        if (tag == entry.tag && product == entry.product) return entry.capability;
    return Capability::Unsupported;
}

struct PacketProof
{
    void Add(Capability capability)
    {
        valid &= capability != Capability::Unsupported;
        visibility |= capability == Capability::Visibility;
        trainedDepth |= capability == Capability::TrainedDepth;
    }
    // Strict local lighting alone does not certify the intersection route.
    bool Complete() const { return valid && visibility && trainedDepth; }
    bool valid = true, visibility = false, trainedDepth = false;
};
inline bool CanEnable(bool published, bool strictRequired, bool certified)
{ return published && (!strictRequired || certified); }

// Rendering callbacks serve strict suppression/restoration as well as trained
// hit/proxy maintenance. Request validation or loading can return before a new
// generation exists, so requested duties must participate independently.
struct SceneDuties
{
    bool requestedStrict=false, requestedTrained=false;
    bool activeStrict=false, pendingStrict=false;
    bool activeRestoration=false, pendingRestoration=false;
    bool activeTrained=false, pendingTrained=false;
    bool TrainedRendering() const { return requestedTrained || activeTrained || pendingTrained; }
    bool NeedsNotifications() const
    {
        return requestedStrict || activeStrict || pendingStrict || activeRestoration || pendingRestoration
            || TrainedRendering();
    }
};
}

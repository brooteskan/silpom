-- SPDX-License-Identifier: MIT
function GetMaterialPropertyDependencies()
    return {"silpomReuseDepthHit", "silpomUseDepthHeightBounds", "silpomUseNeuralSeed", "silpomUseNeuralCache",
        "silpomTowerLod", "isTransparent", "isTintedTransparent", "silpomNeuralVisibilityOnly", "silpomLearnedMaterial", "silpomProxyLod", "silpomSharedVisibility", "silpomProxyDepth", "silpomLightingComparison"}
end

function Process(context)
    if not context:HasShaderWithTag("silpom_hit_depth") then
        return
    end

    local useHit = context:GetMaterialPropertyValue_bool("silpomReuseDepthHit")
        and not context:GetMaterialPropertyValue_bool("isTransparent")
        and not context:GetMaterialPropertyValue_bool("isTintedTransparent")
    local useBounds = context:GetMaterialPropertyValue_bool("silpomUseDepthHeightBounds")
    local useNeural = context:GetMaterialPropertyValue_bool("silpomUseNeuralSeed")
    local useCache = context:GetMaterialPropertyValue_bool("silpomUseNeuralCache")
    local useLod = useNeural and context:GetMaterialPropertyValue_bool("silpomTowerLod")
    context:GetShaderByTag("silpom_hit_depth"):SetEnabled(useHit and not useBounds and not useNeural)
    context:GetShaderByTag("silpom_hit_depth_bounds"):SetEnabled(useHit and useBounds and not useNeural)
    context:GetShaderByTag("silpom_hit_depth_neural"):SetEnabled(useHit and useNeural and not useCache and not useLod)
    context:GetShaderByTag("silpom_hit_depth_neural_cache"):SetEnabled(useHit and useNeural and useCache and not useLod)
    context:GetShaderByTag("silpom_hit_depth_lod_near"):SetEnabled(useHit and useLod)
    context:GetShaderByTag("silpom_hit_depth_lod_shell"):SetEnabled(useHit and useLod)
    context:GetShaderByTag("silpom_hit_depth_wall_backup"):SetEnabled(useHit and useLod)
    local decoded=context:GetMaterialPropertyValue_bool("silpomLearnedMaterial")
    local shared=context:GetMaterialPropertyValue_bool("silpomSharedVisibility")
    local depth=context:GetMaterialPropertyValue_bool("silpomProxyDepth")
    local proxy=context:GetMaterialPropertyValue_bool("silpomProxyLod")
    local visibilityOnly=context:GetMaterialPropertyValue_bool("silpomNeuralVisibilityOnly")
    local comparison=context:GetMaterialPropertyValue_bool("silpomLightingComparison")
    context:GetShaderByTag("silpom_hit_depth_proxy"):SetEnabled(useHit and useLod and proxy and not depth)
    context:GetShaderByTag("silpom_hit_depth_learned_proxy"):SetEnabled(useHit and useLod and proxy and depth)
    context:GetShaderByTag("silpom_hit_forward_level_five"):SetEnabled(useHit and visibilityOnly and proxy and not shared)
    context:GetShaderByTag("silpom_hit_forward_shared_proxy"):SetEnabled(useHit and visibilityOnly and proxy and shared and not comparison)
    context:GetShaderByTag("silpom_hit_forward_shared_proxy_compare"):SetEnabled(useHit and visibilityOnly and proxy and shared and comparison)
    context:GetShaderByTag("silpom_hit_forward"):SetEnabled(useHit and not visibilityOnly and not decoded and not proxy)
    context:GetShaderByTag("silpom_hit_forward_neural_only"):SetEnabled(useHit and visibilityOnly and not decoded and not proxy)
    context:GetShaderByTag("silpom_hit_forward_learned_material"):SetEnabled(useHit and visibilityOnly and decoded and not proxy and not shared)
    context:GetShaderByTag("silpom_hit_forward_shared"):SetEnabled(useHit and visibilityOnly and decoded and not proxy and shared and not comparison)
    context:GetShaderByTag("silpom_hit_forward_shared_compare"):SetEnabled(useHit and visibilityOnly and decoded and not proxy and shared and comparison)
    if useHit then
        context:GetShaderByTag("depth_customZ"):SetEnabled(false)
        context:GetShaderByTag("forward_customZ"):SetEnabled(false)
    end
end

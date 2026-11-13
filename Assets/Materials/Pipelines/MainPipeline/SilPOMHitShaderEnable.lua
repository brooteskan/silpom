-- SPDX-License-Identifier: MIT
function GetMaterialPropertyDependencies()
    return {"silpomReuseDepthHit", "silpomUseDepthHeightBounds", "silpomUseNeuralSeed", "silpomUseNeuralCache",
        "silpomTowerLod", "isTransparent", "isTintedTransparent", "silpomNeuralVisibilityOnly", "silpomLearnedMaterial"}
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
    local visibilityOnly=context:GetMaterialPropertyValue_bool("silpomNeuralVisibilityOnly")
    context:GetShaderByTag("silpom_hit_forward"):SetEnabled(useHit and not visibilityOnly and not decoded)
    context:GetShaderByTag("silpom_hit_forward_neural_only"):SetEnabled(useHit and visibilityOnly and not decoded)
    context:GetShaderByTag("silpom_hit_forward_learned_material"):SetEnabled(useHit and visibilityOnly and decoded)
    if useHit then
        context:GetShaderByTag("depth_customZ"):SetEnabled(false)
        context:GetShaderByTag("forward_customZ"):SetEnabled(false)
    end
end

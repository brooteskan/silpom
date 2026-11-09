-- SPDX-License-Identifier: MIT
function GetMaterialPropertyDependencies()
    return {"silpomReuseDepthHit", "silpomUseDepthHeightBounds", "silpomUseNeuralSeed", "silpomUseNeuralCache",
        "silpomTowerLod", "isTransparent", "isTintedTransparent"}
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
    context:GetShaderByTag("silpom_hit_forward"):SetEnabled(useHit)
    if useHit then
        context:GetShaderByTag("depth_customZ"):SetEnabled(false)
        context:GetShaderByTag("forward_customZ"):SetEnabled(false)
    end
end

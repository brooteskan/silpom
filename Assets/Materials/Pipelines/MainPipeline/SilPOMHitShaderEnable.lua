-- SPDX-License-Identifier: MIT
function GetMaterialPropertyDependencies()
    return {"silpomReuseDepthHit", "isTransparent", "isTintedTransparent"}
end

function Process(context)
    if not context:HasShaderWithTag("silpom_hit_depth") then
        return
    end

    local useHit = context:GetMaterialPropertyValue_bool("silpomReuseDepthHit")
        and not context:GetMaterialPropertyValue_bool("isTransparent")
        and not context:GetMaterialPropertyValue_bool("isTintedTransparent")
    context:GetShaderByTag("silpom_hit_depth"):SetEnabled(useHit)
    context:GetShaderByTag("silpom_hit_forward"):SetEnabled(useHit)
    if useHit then
        context:GetShaderByTag("depth_customZ"):SetEnabled(false)
        context:GetShaderByTag("forward_customZ"):SetEnabled(false)
    end
end

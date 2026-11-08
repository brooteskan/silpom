-- SPDX-License-Identifier: MIT
function GetMaterialPropertyDependencies()
    return {"surface.scale", "mesh.hitFaceBase"}
end

function Process(context)
    context:SetInternalMaterialPropertyValue_bool("hasPerPixelDepth", true)
    context:SetInternalMaterialPropertyValue_bool("silpomReuseDepthHit",
        context:GetMaterialPropertyValue_uint("mesh.hitFaceBase") ~= 0)
end

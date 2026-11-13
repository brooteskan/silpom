-- SPDX-License-Identifier: MIT
function GetMaterialPropertyDependencies()
    return {"surface.scale", "surface.useHierarchy", "surface.useNeuralSeed", "surface.useNeuralCache",
        "surface.neuralModel", "surface.maxCells", "mesh.hitFaceBase", "surface.localShadowMethod", "surface.learnedMaterial"}
end

function Process(context)
    context:SetInternalMaterialPropertyValue_bool("silpomLearnedMaterial",context:GetMaterialPropertyValue_bool("surface.learnedMaterial"))
    context:SetInternalMaterialPropertyValue_bool("silpomNeuralVisibilityOnly",
        context:GetMaterialPropertyValue_enum("surface.localShadowMethod") == 3)
    context:SetInternalMaterialPropertyValue_bool("hasPerPixelDepth", true)
    context:SetInternalMaterialPropertyValue_bool("silpomReuseDepthHit",
        context:GetMaterialPropertyValue_uint("mesh.hitFaceBase") ~= 0)
    context:SetInternalMaterialPropertyValue_bool("silpomUseDepthHeightBounds",
        context:GetMaterialPropertyValue_bool("surface.useHierarchy"))
    context:SetInternalMaterialPropertyValue_bool("silpomUseNeuralSeed",
        context:GetMaterialPropertyValue_bool("surface.useNeuralSeed"))
    context:SetInternalMaterialPropertyValue_bool("silpomUseNeuralCache",
        context:GetMaterialPropertyValue_bool("surface.useNeuralCache"))
    local mode = context:GetMaterialPropertyValue_uint("surface.maxCells")
    local hasLodEstimator = context:GetMaterialPropertyValue_bool("surface.useNeuralCache")
        or context:GetMaterialPropertyValue_uint("surface.neuralModel") ~= 0
    context:SetInternalMaterialPropertyValue_bool("silpomTowerLod", hasLodEstimator and mode >= 65525 and mode <= 65529)
end

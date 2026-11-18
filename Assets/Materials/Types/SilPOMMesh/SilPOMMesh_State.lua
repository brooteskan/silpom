-- SPDX-License-Identifier: MIT
function GetMaterialPropertyDependencies()
    return {"surface.scale", "surface.useHierarchy", "surface.useNeuralSeed", "surface.useNeuralCache",
        "surface.levelTwoPredictor", "surface.validationGather",
        "surface.neuralModel", "surface.reliefMode", "mesh.hitFaceBase", "surface.localShadowMethod", "surface.learnedMaterial", "surface.proxyLodEnabled", "surface.sharedVisibility", "surface.proxyDepthEnabled", "surface.lightingEstimator", "surface.diffuseAccessibility"}
end

function Process(context)
    context:SetInternalMaterialPropertyValue_bool("silpomStaticWallHit",
        context:GetMaterialPropertyValue_uint("surface.neuralModel") == 4
        and context:GetMaterialPropertyValue_bool("surface.levelTwoPredictor")
        and context:GetMaterialPropertyValue_bool("surface.validationGather"))
    context:SetInternalMaterialPropertyValue_bool("silpomDiffuseAccessibility",context:GetMaterialPropertyValue_bool("surface.diffuseAccessibility"))
    context:SetInternalMaterialPropertyValue_bool("silpomAngularEdges",context:GetMaterialPropertyValue_enum("surface.lightingEstimator") == 5)
    context:SetInternalMaterialPropertyValue_bool("silpomLightingComparison",context:GetMaterialPropertyValue_enum("surface.lightingEstimator") ~= 0)
    context:SetInternalMaterialPropertyValue_bool("silpomSharedVisibility",context:GetMaterialPropertyValue_bool("surface.sharedVisibility"))
    context:SetInternalMaterialPropertyValue_bool("silpomProxyDepth",context:GetMaterialPropertyValue_bool("surface.proxyDepthEnabled"))
    context:SetInternalMaterialPropertyValue_bool("silpomProxyLod",context:GetMaterialPropertyValue_bool("surface.proxyLodEnabled"))
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
    -- SilPOM::ReliefMode values 4-8 are the LOD modes (2/4/8 px, shell, 4 px + resolve).
    local mode = context:GetMaterialPropertyValue_uint("surface.reliefMode")
    local hasLodEstimator = context:GetMaterialPropertyValue_bool("surface.useNeuralCache")
        or context:GetMaterialPropertyValue_uint("surface.neuralModel") ~= 0
    context:SetInternalMaterialPropertyValue_bool("silpomReliefLod", hasLodEstimator and mode >= 4 and mode <= 8)
end

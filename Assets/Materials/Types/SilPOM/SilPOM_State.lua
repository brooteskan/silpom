function GetMaterialPropertyDependencies()
    return {"surface.scale"}
end
function Process(context)
    -- All visibility passes must use custom depth even for zero displacement.
    context:SetInternalMaterialPropertyValue_bool("hasPerPixelDepth", true)
end

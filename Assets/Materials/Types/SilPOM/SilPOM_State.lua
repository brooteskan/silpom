function GetMaterialPropertyDependencies()
    return {"surface.scale"}
end
function Process(context)
    -- Camera visibility needs custom depth even at zero displacement. The
    -- SHADOWMAP specialization passes through flat raster depth without tracing.
    context:SetInternalMaterialPropertyValue_bool("hasPerPixelDepth", true)
end

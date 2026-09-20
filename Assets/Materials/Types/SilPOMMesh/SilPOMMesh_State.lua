function GetMaterialPropertyDependencies()
    return {"mesh.scale"}
end
function Process(context)
    context:SetInternalMaterialPropertyValue_bool("hasPerPixelDepth", true)
end

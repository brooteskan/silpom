"""Import the authored SurfaceTexture stamps into the painter's asset library."""
import json
from pathlib import Path
import shutil

project = Path(__file__).resolve().parents[3]
source = project.parent / "assets-source/Textures/Decals"
destination = project / "Assets/Materials/DecalPaint"

for package in sorted(source.glob("SurfaceTexture_*")):
    name = package.name.lower()
    folder = destination / name
    folder.mkdir(parents=True, exist_ok=True)
    for suffix, original, preset in (
        ("", "BaseColor_RGBA", "Decal_AlbedoWithOpacity"),
        ("_normal", "Normal_DirectX", "Normals"),
    ):
        target = folder / f"{name}{suffix}.png"
        shutil.copyfile(package / "Textures" / f"SurfaceTexture_{original}.png", target)
        settings = {"Type": "JsonSerialization", "Version": 1, "ClassName": "TextureSettings",
                    "ClassData": {"Preset": preset, "SizeReduceLevel": 0,
                                  "EnableMipmap": True, "MaintainAlphaCoverage": False}}
        target.with_suffix(".png.assetinfo").write_text(json.dumps(settings, indent=4) + "\n", encoding="utf-8")
    shutil.copyfile(package / "Stamp_Preview.png", folder / f"{name}.preview.png")
    material = {
        "materialType": "@gemroot:Atom_Feature_Common@/Assets/Materials/Types/StandardPBR.materialtype",
        "materialTypeVersion": 5,
        "propertyValues": {"baseColor.textureMap": f"{name}.png",
                           "normal.textureMap": f"{name}_normal.png"},
    }
    (folder / f"{name}.material").write_text(json.dumps(material, indent=4) + "\n", encoding="utf-8")
    print(f"Imported {package.name}: {folder}")

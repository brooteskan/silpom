"""Import authored SurfaceTexture stamp packages into a project's painter asset library.

python ImportSurfaceDecals.py --source <folder of SurfaceTexture_* packages> --project <O3DE project>
writes <project>/Assets/Materials/DecalPaint/<name>/. Each package holds
Textures/SurfaceTexture_BaseColor_RGBA.png, Textures/SurfaceTexture_Normal_DirectX.png
and Stamp_Preview.png.
"""
import argparse
import json
from pathlib import Path
import shutil

cli = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
cli.add_argument("--source", type=Path, required=True)
cli.add_argument("--project", type=Path, required=True)
args = cli.parse_args()
source = args.source.resolve()
destination = args.project.resolve() / "Assets/Materials/DecalPaint"

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

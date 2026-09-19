"""Create comparison materials using TG's textures, without modifying the originals.

Run with ordinary Python: python prepare_tg_fixture.py --project D:/TG/TGProject
The copied height image is cooked as normalized linear, single-mip R32_FLOAT.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


def prepare(project):
    source = project / "Assets/Materials/StoneWall"
    target = project / "Assets/SilPOM"
    target.mkdir(parents=True, exist_ok=True)
    height = source / "stonewall_height.png"
    shutil.copyfile(height, target / height.name)
    (target / (height.name + ".assetinfo")).write_text('''<ObjectStream version="3">
  <Class name="TextureSettings" version="2" type="{980132FF-C450-425D-8AE0-BD96A8486177}">
    <Class name="Name" field="Preset" value="LUT_R32F" type="{3D2B920C-9EFD-40D5-AAE0-DF131C3D4931}"/>
    <Class name="unsigned int" field="SizeReduceLevel" value="0" type="{43DA906B-7DEF-4CA8-9790-854106D3F983}"/>
    <Class name="bool" field="EngineReduce" value="true" type="{A0CA880C-AFE4-43CB-926C-59AC48496112}"/>
    <Class name="bool" field="EnableMipmap" value="false" type="{A0CA880C-AFE4-43CB-926C-59AC48496112}"/>
  </Class>
</ObjectStream>
''', encoding="utf-8")
    properties = {
        "baseColor.textureMap": "../Materials/StoneWall/stonewall_basecolor.png",
        "normal.textureMap": "../Materials/StoneWall/stonewall_normal.png",
        "normal.flipY": True,
        "occlusion.diffuseTextureMap": "../Materials/StoneWall/stonewall_ao.png",
        "roughness.factor": 0.65,
        "general.doubleSided": True,
    }
    material = {
        "materialType": "@gemroot:SilPOM@/Assets/Materials/Types/SilPOM/SilPOM.materialtype",
        "materialTypeVersion": 1,
        "propertyValues": {**properties, "surface.heightMap": height.name},
    }
    (target / "StoneWall.material").write_text(json.dumps(material, indent=4) + "\n")
    baseline = {
        "materialType": "@gemroot:Atom_Feature_Common@/Assets/Materials/Types/StandardPBR.materialtype",
        "materialTypeVersion": 5,
        "propertyValues": {**properties, "parallax.textureMap": height.name,
                           "parallax.factor": 0.1, "parallax.offset": 0.5, "parallax.pdo": True,
                           "parallax.algorithm": "ContactRefinement", "parallax.quality": "High"},
    }
    (target / "StoneWallPOM.material").write_text(json.dumps(baseline, indent=4) + "\n")
    provenance = {"surface_version": 1, "height_sha256": hashlib.sha256(height.read_bytes()).hexdigest(),
                  "source": str(height), "height_encoding": "linear R32_FLOAT, mip zero, bilinear",
                  "geometry_bounds": [0, 1], "generated_files": ["StoneWall.material", "StoneWallPOM.material", height.name]}
    (target / "fixture_provenance.json").write_text(json.dumps(provenance, indent=4) + "\n")
    print(json.dumps(provenance, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", type=Path, required=True)
    prepare(parser.parse_args().project.resolve())

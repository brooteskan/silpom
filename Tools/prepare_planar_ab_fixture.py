"""Generate matched planar A/B materials; leaves original TG assets untouched."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct


def prepare(project):
    source = project / "Assets/Materials/StoneWall"
    reference_source = source / "M_StoneWall_Trim_Displacement.material"
    reference = json.loads(reference_source.read_text(encoding="utf-8"))["propertyValues"]
    target = project / "Assets/SilPOMPlanarAB"
    target.mkdir(parents=True, exist_ok=True)
    height = source / reference["vertexDisplacement.textureMap"]
    copied = target / "height.png"
    shutil.copyfile(height, copied)
    width, height_pixels = struct.unpack(">II", copied.read_bytes()[16:24])
    (target / "height.png.assetinfo").write_text('''<ObjectStream version="3">
  <Class name="TextureSettings" version="2" type="{980132FF-C450-425D-8AE0-BD96A8486177}">
    <Class name="Name" field="Preset" value="LUT_R32F" type="{3D2B920C-9EFD-40D5-AAE0-DF131C3D4931}"/>
    <Class name="unsigned int" field="SizeReduceLevel" value="0" type="{43DA906B-7DEF-4CA8-9790-854106D3F983}"/>
    <Class name="bool" field="EngineReduce" value="false" type="{A0CA880C-AFE4-43CB-926C-59AC48496112}"/>
    <Class name="bool" field="EnableMipmap" value="false" type="{A0CA880C-AFE4-43CB-926C-59AC48496112}"/>
  </Class>
</ObjectStream>
''', encoding="utf-8")
    common = {key: value for key, value in reference.items()
              if not key.startswith(("vertexDisplacement.", "parallax."))}
    for key in ("baseColor.textureMap", "normal.textureMap", "occlusion.diffuseTextureMap"):
        if key in common:
            common[key] = "../Materials/StoneWall/" + common[key]
    common.update({"general.doubleSided": True, "parallax.useTexture": False, "parallax.pdo": False})
    scale = reference.get("vertexDisplacement.scale", .1)
    midpoint = reference.get("vertexDisplacement.midpoint", .5)
    def material(name, kind, props):
        document = {"materialType": kind, "materialTypeVersion": 1, "propertyValues": props}
        (target / name).write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    material("Quad.material", "@gemroot:SilPOM@/Assets/Materials/Types/SilPOM/SilPOM.materialtype",
             {**common, "surface.heightMap": "height.png", "surface.scale": scale,
              "surface.reference": midpoint, "surface.addressMode": 0, "surface.maxCells": 4096})
    material("Reference.material", "../Materials/Types/DisplacementPBR/DisplacementPBR.materialtype",
             {**common, "general.castShadows": False, "vertexDisplacement.textureMap": "height.png",
              "vertexDisplacement.textureMapUv": "Unwrapped", "vertexDisplacement.useTexture": True,
              "vertexDisplacement.scale": scale, "vertexDisplacement.midpoint": midpoint,
              "vertexDisplacement.lod": 0.0})
    manifest = {
        "schema": 1, "width_metres": 2.0, "height_metres": 2.0, "scale_metres": scale, "midpoint": midpoint,
        "height_size": [width, height_pixels], "height_sha256": hashlib.sha256(copied.read_bytes()).hexdigest(),
        "height_encoding": "linear single-mip R32_FLOAT; bilinear texel centers; repeat",
        "uv": {"tile": [1, 1], "offset": [0, 0], "rotation": 0}, "max_cells": 4096,
        "reference_source": str(reference_source), "reference_source_sha256": hashlib.sha256(reference_source.read_bytes()).hexdigest(),
        "reference_material": "assets/silpomplanarab/reference.azmaterial",
        "quad_material": "assets/silpomplanarab/quad.azmaterial",
        "subdivision_candidates": [32, 64, 128, 256, 512], "resolution": [1920, 1080], "msaa": 2,
        "samples": 600, "runs": 3, "warmup_frames": 120,
        "thresholds": {"agreed": False, "median_improvement": .20, "p95_regression": .05,
                       "silhouette_p95_pixels": 1.0, "depth_p95_metres": .001, "ssim": .98},
        "shadow": "same undisplaced two-triangle SilPOM quad for both variants",
        "common_material": common,
    }
    (target / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2))
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    prepare(parser.parse_args().project.resolve())

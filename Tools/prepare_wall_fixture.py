"""Create the wall comparison materials used by Tests/editor_smoke.py.

python Tools/prepare_wall_fixture.py [--project <O3DE project>] [--height ... --basecolor ... --normal ...]

Writes <project>/Assets/SilPOM/StoneWall.material (SilPOM) and StoneWallPOM.material
(StandardPBR parallax baseline) plus fixture_provenance.json. Without texture
arguments it uses the fixture project's Irish fieldstone wall textures. Source
textures are referenced in place; a height image without a single-mip LUT_R32F
.assetinfo is copied next to the materials with one. Originals are never modified.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from silpom_project import FIXTURE_PROJECT, add_project_argument

WALL = FIXTURE_PROJECT / "Assets/SilPOMPlanarAB/IrishFieldstoneTowerWelded/wall"
LUT_ASSETINFO = '''<ObjectStream version="3">
  <Class name="TextureSettings" version="2" type="{980132FF-C450-425D-8AE0-BD96A8486177}">
    <Class name="Name" field="Preset" value="LUT_R32F" type="{3D2B920C-9EFD-40D5-AAE0-DF131C3D4931}"/>
    <Class name="unsigned int" field="SizeReduceLevel" value="0" type="{43DA906B-7DEF-4CA8-9790-854106D3F983}"/>
    <Class name="bool" field="EngineReduce" value="true" type="{A0CA880C-AFE4-43CB-926C-59AC48496112}"/>
    <Class name="bool" field="EnableMipmap" value="false" type="{A0CA880C-AFE4-43CB-926C-59AC48496112}"/>
  </Class>
</ObjectStream>
'''


def relative(path, folder):
    return Path(os.path.relpath(path, folder)).as_posix()


def single_mip_lut(height):
    info = height.with_name(height.name + ".assetinfo")
    text = info.read_text(encoding="utf-8") if info.is_file() else ""
    return 'value="LUT_R32F"' in text and 'field="EnableMipmap" value="false"' in text


def prepare(project, height, basecolor, normal, ao=None, roughness=None, flip_y=True, roughness_factor=0.65):
    target = project / "Assets/SilPOM"
    target.mkdir(parents=True, exist_ok=True)
    for source in (height, basecolor, normal, ao, roughness):
        if source is not None and not source.resolve().is_relative_to(project.resolve()):
            raise ValueError(f"{source} must be inside the project so Asset Processor can resolve it")
    if single_mip_lut(height):
        height_reference = relative(height, target)
        generated = []
    else:
        shutil.copyfile(height, target / height.name)
        (target / (height.name + ".assetinfo")).write_text(LUT_ASSETINFO, encoding="utf-8")
        height_reference = height.name
        generated = [height.name]
    properties = {
        "baseColor.textureMap": relative(basecolor, target),
        "normal.textureMap": relative(normal, target),
        "normal.flipY": flip_y,
        "general.doubleSided": True,
    }
    if ao is not None:
        properties["occlusion.diffuseTextureMap"] = relative(ao, target)
    if roughness is not None:
        properties["roughness.textureMap"] = relative(roughness, target)
    else:
        properties["roughness.factor"] = roughness_factor
    material = {
        "materialType": "@gemroot:SilPOM@/Assets/Materials/Types/SilPOM/SilPOM.materialtype",
        "materialTypeVersion": 1,
        "propertyValues": {**properties, "surface.heightMap": height_reference},
    }
    (target / "StoneWall.material").write_text(json.dumps(material, indent=4) + "\n")
    baseline = {
        "materialType": "@gemroot:Atom_Feature_Common@/Assets/Materials/Types/StandardPBR.materialtype",
        "materialTypeVersion": 5,
        "propertyValues": {**properties, "parallax.textureMap": height_reference,
                           "parallax.factor": 0.1, "parallax.offset": 0.5, "parallax.pdo": True,
                           "parallax.algorithm": "ContactRefinement", "parallax.quality": "High"},
    }
    (target / "StoneWallPOM.material").write_text(json.dumps(baseline, indent=4) + "\n")
    provenance = {"surface_version": 1, "height_sha256": hashlib.sha256(height.read_bytes()).hexdigest(),
                  "source": relative(height, project), "height_encoding": "linear R32_FLOAT, mip zero, bilinear",
                  "geometry_bounds": [0, 1],
                  "generated_files": ["StoneWall.material", "StoneWallPOM.material", *generated]}
    (target / "fixture_provenance.json").write_text(json.dumps(provenance, indent=4) + "\n")
    print(json.dumps(provenance, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_project_argument(parser)
    parser.add_argument("--height", type=Path, default=WALL / "height.png")
    parser.add_argument("--basecolor", type=Path, default=WALL / "basecolor.png")
    parser.add_argument("--normal", type=Path, default=WALL / "normal.png")
    parser.add_argument("--roughness", type=Path, default=WALL / "roughness.png")
    parser.add_argument("--ao", type=Path)
    parser.add_argument("--no-flip-y", dest="flip_y", action="store_false")
    args = parser.parse_args()
    prepare(args.project.resolve(), args.height.resolve(), args.basecolor.resolve(), args.normal.resolve(),
            args.ao.resolve() if args.ao else None, args.roughness.resolve() if args.roughness else None, args.flip_y)

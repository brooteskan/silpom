"""Reproduce the small imported curved mesh using the actual Blender add-on.

blender --background --factory-startup --python-exit-code 1 --python
Tests/create_mesh_fixture.py -- --output <empty fixture asset directory>
"""
import argparse
import json
from pathlib import Path
import sys
import tempfile
import addon_utils
import bmesh
import bpy

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "Tools"))
from package_blender_addon import package


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        raise RuntimeError("Use an empty output directory; fixture generation does not replace existing assets")
    output.mkdir(parents=True, exist_ok=True)
    bpy.context.preferences.use_preferences_save = False
    with tempfile.TemporaryDirectory(prefix="silpom-fixture-") as temporary:
        work = Path(temporary)
        archive = package(work / "silpom_exporter.zip")
        assert bpy.ops.preferences.script_directory_add(directory=str(work / "scripts")) == {"FINISHED"}
        scripts = bpy.context.preferences.filepaths.script_directories[-1]
        scripts.name = "SilPOM Fixture"; scripts.directory = str(work / "scripts")
        (work / "scripts/addons").mkdir(parents=True)
        assert bpy.ops.preferences.addon_install(filepath=str(archive), target=scripts.name) == {"FINISHED"}
        addon = addon_utils.enable("silpom_exporter", default_set=False)
        assert addon is not None
        bpy.ops.wm.open_mainfile(filepath=str(ROOT / "Tests/Fixtures/RoundTrip/roundtrip.blend"))
        obj = next(o for o in bpy.context.scene.objects if o.type == "MESH")
        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True); bpy.context.view_layer.objects.active = obj
        bpy.ops.object.mode_set(mode="EDIT")
        bm = bmesh.from_edit_mesh(obj.data); bm.faces.ensure_lookup_table()
        for face in bm.faces: face.select_set(True)
        bmesh.update_edit_mesh(obj.data)
        assert bpy.ops.silpom.assign_region(ordinary=True) == {"FINISHED"}
        for face in bm.faces: face.select_set(face.index != 3)
        bmesh.update_edit_mesh(obj.data)
        bpy.context.scene.silpom_export.region = 1
        bpy.context.scene.silpom_export.profile = 1
        assert bpy.ops.silpom.assign_region() == {"FINISHED"}
        bpy.ops.object.mode_set(mode="OBJECT")
        assert bpy.ops.export_scene.silpom(filepath=str(output / "curved.fbx")) == {"FINISHED"}
        image = bpy.data.images.new("SilPOM height", width=2, height=2, float_buffer=True)
        image.colorspace_settings.name = "Non-Color"
        image.pixels[:] = [c for h in (0.125, 0.875, 0.75, 0.25) for c in (h, h, h, 1)]
        image.filepath_raw = str(output / "height.png"); image.file_format = "PNG"; image.save()
        (output / "height.png.assetinfo").write_text('''<ObjectStream version="3">
  <Class name="TextureSettings" version="2" type="{980132FF-C450-425D-8AE0-BD96A8486177}">
    <Class name="Name" field="Preset" value="LUT_R32F" type="{3D2B920C-9EFD-40D5-AAE0-DF131C3D4931}"/>
    <Class name="unsigned int" field="SizeReduceLevel" value="0" type="{43DA906B-7DEF-4CA8-9790-854106D3F983}"/>
    <Class name="bool" field="EngineReduce" value="false" type="{A0CA880C-AFE4-43CB-926C-59AC48496112}"/>
    <Class name="bool" field="EnableMipmap" value="false" type="{A0CA880C-AFE4-43CB-926C-59AC48496112}"/>
  </Class>
</ObjectStream>
''', encoding="utf-8")
        sidecar = json.loads((output / "curved.silpom.json").read_text())
        binding = {}
        for i, material in enumerate(sidecar["materials"]):
            name = f"material_{i}.material"
            (output / name).write_text(json.dumps({
                "materialType": "@gemroot:Atom_Feature_Common@/Assets/Materials/Types/StandardPBR.materialtype",
                "materialTypeVersion": 5,
                "propertyValues": {"baseColor.color": [0.65, 0.15, 0.08, 1] if i == 0 else [0.08, 0.35, 0.7, 1],
                                   "roughness.factor": 0.65, "general.doubleSided": True}}, indent=2))
            binding[material["id"]] = name
        report = {"materials": binding, "profiles": {"1": "height.png"},
                  "faces": len(sidecar["faces"]), "selected": sum(bool(f["region"]) for f in sidecar["faces"]),
                  "blender": bpy.app.version_string, "exported_through_addon": True}
        (output / "fixture.json").write_text(json.dumps(report, indent=2))
        addon_utils.disable("silpom_exporter", default_set=False)
        print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

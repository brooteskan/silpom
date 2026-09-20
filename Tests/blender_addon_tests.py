"""Install and exercise the packaged add-on in background Blender, without saving preferences."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
import zipfile

import addon_utils
import bmesh
import bpy

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'Tools'))
from package_blender_addon import package


def snapshot():
    return {
        'data': {name: sorted(item.name for item in getattr(bpy.data, name))
                 for name in ('objects', 'meshes', 'materials', 'collections')},
        'selection': sorted(obj.name for obj in bpy.context.selected_objects),
        'active': bpy.context.view_layer.objects.active.name if bpy.context.view_layer.objects.active else None,
        'units': bpy.context.scene.unit_settings.scale_length,
        'mode': bpy.context.mode,
        'file': bpy.data.filepath,
    }


def rejected(call, message):
    try:
        result = call()
    except RuntimeError as error:  # bpy.ops raises when an operator reports ERROR.
        assert message in str(error), str(error)
    else:
        assert result == {'CANCELLED'}, result


def load():
    bpy.ops.wm.open_mainfile(filepath=str(ROOT / 'Tests/Fixtures/RoundTrip/roundtrip.blend'))
    obj = next(obj for obj in bpy.context.scene.objects if obj.type == 'MESH')
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    return obj


def select_faces(obj, indices):
    if bpy.context.mode != 'EDIT_MESH':
        bpy.ops.object.mode_set(mode='EDIT')
    bm = bmesh.from_edit_mesh(obj.data)
    bm.faces.ensure_lookup_table()
    for face in bm.faces:
        face.select_set(face.index in indices)
    bmesh.update_edit_mesh(obj.data, loop_triangles=False, destructive=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    checks = []
    bpy.context.preferences.use_preferences_save = False

    with tempfile.TemporaryDirectory(prefix='session-', dir=output) as temporary:
        work = Path(temporary)
        archive = package(work / 'silpom_exporter.zip')
        assert archive.read_bytes() == package(work / 'second.zip').read_bytes()
        with zipfile.ZipFile(archive) as bundled:
            assert set(bundled.namelist()) == {'silpom_exporter/__init__.py', 'silpom_exporter/export_mesh.py', 'silpom_exporter/LICENSE'}
            assert bundled.read('silpom_exporter/export_mesh.py') == (ROOT / 'Tools/export_mesh.py').read_bytes()
        checks.append('reproducible-self-contained-package')

        # Install through Blender's real ZIP installer into a disposable script path.
        # Never install into, remove from, or save the user's Blender preferences.
        assert bpy.ops.preferences.script_directory_add(directory=str(work / 'scripts')) == {'FINISHED'}
        scripts = bpy.context.preferences.filepaths.script_directories[-1]
        scripts.name = 'SilPOM Isolated Test'
        scripts.directory = str(work / 'scripts')
        (work / 'scripts/addons').mkdir(parents=True)
        assert bpy.ops.preferences.addon_install(filepath=str(archive), target=scripts.name) == {'FINISHED'}
        addon = addon_utils.enable('silpom_exporter', default_set=False)
        assert addon is not None and hasattr(bpy.types.Scene, 'silpom_export')
        assert Path(addon.__file__).is_relative_to(work)
        checks.append('zip-install-and-enable')

        obj = load()
        obj.data.attributes.remove(obj.data.attributes['silpom_region_id'])
        select_faces(obj, {0, 2})
        bpy.context.scene.silpom_export.region = 7
        bpy.context.scene.silpom_export.profile = 3
        assert bpy.ops.silpom.assign_region() == {'FINISHED'}
        select_faces(obj, {2})
        assert bpy.ops.silpom.assign_region(ordinary=True) == {'FINISHED'}
        bpy.ops.object.mode_set(mode='OBJECT')
        assert [v.value for v in obj.data.attributes['silpom_region_id'].data] == [7, 0, 0, 0]
        assert [v.value for v in obj.data.attributes['silpom_profile_id'].data] == [3, 0, 0, 0]
        checks.append('assign-region-profile-and-ordinary-faces')

        select_faces(obj, set())
        rejected(lambda: bpy.ops.silpom.assign_region(), 'Select at least one face')
        checks.append('empty-face-selection-rejected')
        select_faces(obj, {0})
        bpy.context.scene.unit_settings.scale_length = .01
        before = snapshot()
        source_hash = hashlib.sha256(Path(bpy.data.filepath).read_bytes()).hexdigest()
        target = work / 'named asset.fbx'
        assert bpy.ops.export_scene.silpom(filepath=str(target)) == {'FINISHED'}
        assert snapshot() == before
        assert hashlib.sha256(Path(bpy.data.filepath).read_bytes()).hexdigest() == source_hash
        doc = json.loads(target.with_suffix('.silpom.json').read_text())
        assert doc['version'] == 2 and len(doc['faces']) == 4 and len(doc['objects']) == 1
        assert sum(bool(f['region']) for f in doc['faces']) == 1
        assert doc['fbx_sha256'] == hashlib.sha256(target.read_bytes()).hexdigest()
        assert target.with_suffix('.authoring.blend').is_file()
        checks.append('edit-mode-export-restores-scene-and-source')

        files = list(addon.output_paths(target.parent, target.stem).values())
        original_bytes = [path.read_bytes() for path in files]
        rejected(lambda: bpy.ops.export_scene.silpom(filepath=str(target)), 'already exist')
        assert snapshot() == before and [path.read_bytes() for path in files] == original_bytes
        assert bpy.ops.export_scene.silpom(filepath=str(target), overwrite=True, initialize_identities=False) == {'FINISHED'}
        assert snapshot() == before
        assert json.loads(target.with_suffix('.silpom.json').read_text())['semantic_sha256'] == doc['semantic_sha256']
        original_bytes = [path.read_bytes() for path in files]
        checks.append('overwrite-guard-and-repeat-export-with-stable-ids')

        # A save-copy must contain original authoring data, not temporary export nodes.
        bpy.ops.object.mode_set(mode='OBJECT')
        bpy.ops.wm.open_mainfile(filepath=str(target.with_suffix('.authoring.blend')))
        assert snapshot()['data'] == before['data']
        assert bpy.context.scene.unit_settings.scale_length == before['units']
        rejected(lambda: bpy.ops.export_scene.silpom(filepath=str(target), overwrite=True), 'input authoring file is never overwritten')
        checks.append('clean-authoring-copy-and-source-overwrite-guard')

        obj = load()
        duplicate = obj.copy()
        duplicate.data = obj.data.copy()
        duplicate.location.x += 4
        bpy.context.collection.objects.link(duplicate)
        duplicate.select_set(False)
        bpy.context.view_layer.update()
        single = work / 'single.fbx'
        assert bpy.ops.export_scene.silpom(filepath=str(single)) == {'FINISHED'}
        assert len(json.loads(single.with_suffix('.silpom.json').read_text())['objects']) == 1
        all_meshes = work / 'all.fbx'
        assert bpy.ops.export_scene.silpom(filepath=str(all_meshes), use_selection=False) == {'FINISHED'}
        all_doc = json.loads(all_meshes.with_suffix('.silpom.json').read_text())
        assert len(all_doc['objects']) == 2 and len({o['id'] for o in all_doc['objects']}) == 2
        checks.append('selected-vs-all-meshes-and-duplicate-id-repair')

        duplicate.select_set(True)
        select_faces(obj, {1})
        select_faces(duplicate, {2})
        bpy.context.scene.silpom_export.region = 12
        bpy.context.scene.silpom_export.profile = 5
        assert bpy.ops.silpom.assign_region() == {'FINISHED'}
        bpy.ops.object.mode_set(mode='OBJECT')
        assert obj.data.attributes['silpom_region_id'].data[1].value == 12
        assert duplicate.data.attributes['silpom_region_id'].data[2].value == 12
        assert obj.data.attributes['silpom_profile_id'].data[0].value == 1
        checks.append('multi-object-edit-and-implicit-profile-preservation')

        obj = load()
        obj.data.attributes.remove(obj.data.attributes['silpom_region_id'])
        wrong = obj.data.attributes.new('silpom_region_id', 'FLOAT', 'FACE')
        select_faces(obj, {0})
        rejected(lambda: bpy.ops.silpom.assign_region(), 'must be Integer / Face')
        bpy.ops.object.mode_set(mode='OBJECT')
        assert obj.data.attributes['silpom_region_id'].data_type == 'FLOAT'
        assert obj.data.attributes.get('silpom_profile_id') is None
        checks.append('invalid-tag-attribute-rejected-without-replacement')

        obj = load()
        obj.data.attributes.remove(obj.data.attributes['silpom_vertex_id'])
        obj.data.attributes.remove(obj.data.attributes['silpom_face_id'])
        del obj['silpom_object_id']
        obj.modifiers.new('Unbaked', 'SUBSURF')
        before = snapshot()
        rejected(lambda: bpy.ops.export_scene.silpom(filepath=str(work / 'invalid.fbx')), 'modifiers require explicit baked topology')
        assert snapshot() == before and 'silpom_object_id' not in obj
        obj.modifiers.clear()
        # Fail after ID allocation and temporary FBX meshes exist.
        core = sys.modules['silpom_exporter.export_mesh']
        real_bpy = core.bpy

        def failing_fbx(**_kwargs):
            assert 'SilPOM_Selected' in bpy.data.objects
            raise RuntimeError('simulated FBX failure')

        proxy = SimpleNamespace(context=bpy.context, data=bpy.data, app=bpy.app,
                                ops=SimpleNamespace(export_scene=SimpleNamespace(fbx=failing_fbx), wm=bpy.ops.wm))
        with patch.object(core, 'bpy', proxy):
            rejected(lambda: bpy.ops.export_scene.silpom(filepath=str(target), overwrite=True), 'simulated FBX failure')
        assert core.bpy is real_bpy and snapshot() == before
        assert 'silpom_object_id' not in obj
        assert obj.data.attributes.get('silpom_vertex_id') is None
        assert obj.data.attributes.get('silpom_face_id') is None
        assert not any(work.glob('.silpom-*'))
        # Existing output survived the failed re-export, byte-for-byte.
        assert [path.read_bytes() for path in files] == original_bytes
        checks.append('validation-and-fbx-failure-cleanup-with-id-rollback')

        bpy.ops.object.select_all(action='DESELECT')
        rejected(lambda: bpy.ops.export_scene.silpom(filepath=str(work / 'empty.fbx')), 'Select at least one mesh')
        checks.append('empty-object-selection-rejected')

        # Keep a tagged GUI-produced authoring copy for optional real O3DE import.
        obj = load()
        select_faces(obj, {0, 1})
        bpy.context.scene.silpom_export.region = 4
        bpy.context.scene.silpom_export.profile = 2
        assert bpy.ops.silpom.assign_region() == {'FINISHED'}
        integration = output / 'integration/roundtrip.fbx'
        assert bpy.ops.export_scene.silpom(filepath=str(integration), overwrite=True) == {'FINISHED'}
        bpy.ops.object.mode_set(mode='OBJECT')
        addon_utils.disable('silpom_exporter', default_set=False)
        assert not hasattr(bpy.types.Scene, 'silpom_export')
        assert addon_utils.enable('silpom_exporter', default_set=False) is not None
        addon_utils.disable('silpom_exporter', default_set=False)
        checks.append('disable-and-reenable')
        bpy.ops.preferences.script_directory_remove(index=len(bpy.context.preferences.filepaths.script_directories) - 1)

    report = {'passed': True, 'blender': bpy.app.version_string, 'checks': checks,
              'integration_blend': str(integration.with_suffix('.authoring.blend'))}
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('SILPOM_ADDON_TESTS ' + json.dumps(report))


if __name__ == '__main__':
    main()

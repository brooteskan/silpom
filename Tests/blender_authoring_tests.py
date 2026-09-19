"""Run in background Blender; creates reserved outputs under --output."""
import argparse
import json
from pathlib import Path
import sys
import bpy

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'Tools'))
from export_mesh import export


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    output = args.output.resolve()
    source = ROOT / 'Tests/Fixtures/RoundTrip/roundtrip.blend'
    checks = []

    def load(path):
        bpy.ops.wm.open_mainfile(filepath=str(path))
        return [o for o in bpy.context.scene.objects if o.type == 'MESH']

    def document(name):
        return json.loads((output / name / 'roundtrip.silpom.json').read_text())

    def reject(name, objects, message):
        try:
            export(output / name, objects)
        except ValueError as error:
            assert message in str(error), (message, str(error))
            checks.append(name)
        else:
            raise AssertionError('Accepted invalid authoring input: ' + name)

    objects = load(source)
    reject('missing-identities', objects, 'silpom_material_id')
    export(output / 'seed', objects, initialize=True)
    seed = output / 'seed/authoring.blend'
    baseline = document('seed')
    checks.append('initialize-and-save-copy')

    objects = load(seed)
    export(output / 'reordered', objects, reorder=True)
    assert document('reordered')['semantic_sha256'] == baseline['semantic_sha256']
    checks.append('persistent-identities-and-reorder')

    objects = load(seed)
    for material in objects[0].data.materials:
        material.name += '_Renamed'
    export(output / 'renamed', objects)
    assert {m['id'] for m in document('renamed')['materials']} == {m['id'] for m in baseline['materials']}
    checks.append('stable-material-rename')

    objects = load(seed)
    original = objects[0]
    field = original.data.attributes.new('silpom_direction', 'FLOAT_VECTOR', 'POINT')
    for index, entry in enumerate(field.data):
        entry.vector = (.1 * index, .05 * index, 1)
    profiles = original.data.attributes.new('silpom_profile_id', 'INT', 'FACE')
    for index, entry in enumerate(profiles.data):
        entry.value = index + 1 if index < 3 else 0
    original.data.attributes['silpom_region_id'].data[1].value = 13
    duplicate = original.copy()
    duplicate.data = original.data.copy()
    duplicate.location.x += 4
    bpy.context.collection.objects.link(duplicate)
    objects.append(duplicate)
    bpy.context.view_layer.update()
    reject('duplicate-object-id', objects, 'silpom_object_id')
    export(output / 'duplicate', objects, initialize=True)
    duplicated = document('duplicate')
    assert len({o['id'] for o in duplicated['objects']}) == 2
    assert len({f['stable_id'] for f in duplicated['faces']}) == 8
    assert len({tuple(d) for f in duplicated['faces'] for d in f['directions']}) > 1
    assert len({f['profile'] for f in duplicated['faces'] if f['region']}) == 3
    checks.append('duplicate-repair-varying-directions-profiles')

    objects = load(seed)
    objects[0].scale = (1, 2, 1)
    bpy.context.view_layer.update()
    reject('nonuniform-scale', objects, 'positive uniform scale')
    objects = load(seed)
    objects[0].modifiers.new('Unsupported', 'SUBSURF')
    reject('modifier', objects, 'modifiers require explicit baked topology')

    objects = load(seed)
    mesh = bpy.data.meshes.new('Quad')
    mesh.from_pydata([(3, 0, 0), (4, 0, 0), (4, 1, .2), (3, 1, 0)], [], [(0, 1, 2, 3)])
    mesh.materials.append(objects[0].data.materials[0])
    region = mesh.attributes.new('silpom_region_id', 'INT', 'FACE')
    region.data[0].value = 3
    uv = mesh.uv_layers.new(name='UVMap')
    for loop, value in zip(uv.data, [(0, 0), (1, 0), (1, 1), (0, 1)]):
        loop.uv = value
    obj = bpy.data.objects.new('Quad', mesh)
    bpy.context.collection.objects.link(obj)
    export(output / 'quad', [obj], initialize=True)
    quad = document('quad')
    assert len(quad['faces']) == 2 and len({f['stable_id'] for f in quad['faces']}) == 2
    checks.append('triangulation-before-partition')
    report = {'passed': True, 'checks': checks, 'integration_blend': str(output / 'duplicate/authoring.blend')}
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print('SILPOM_AUTHORING_TESTS ' + json.dumps(report))


if __name__ == '__main__':
    main()

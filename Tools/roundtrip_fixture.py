"""Blender background entry point: --python this.py -- --output DIR [--reorder].

Build an isolated authoring fixture, capture identities before partitioning,
export selected/ordinary nodes once, and bind the sidecar to the exact FBX bytes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

import bpy


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reorder', action='store_true')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    vertices = [(-1, -1, 0), (-1, 1, 0), (0, -1, .25), (0, 1, .25), (1, -1, 0), (1, 1, 0)]
    faces = [(0, 2, 1), (2, 3, 1), (2, 4, 3), (4, 5, 3)]
    materials = [bpy.data.materials.new('SilPOM_Material_A'), bpy.data.materials.new('SilPOM_Material_B')]
    mesh = bpy.data.meshes.new('RoundTripAuthoringMesh')
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    obj = bpy.data.objects.new('RoundTripAuthoring', mesh)
    bpy.context.collection.objects.link(obj)
    obj['silpom_object_id'] = 'fixture-object-001'
    for mat in materials:
        mesh.materials.append(mat)
    face_ids = mesh.attributes.new('silpom_face_id', 'INT', 'FACE')
    regions = mesh.attributes.new('silpom_region_id', 'INT', 'FACE')
    vertex_ids = mesh.attributes.new('silpom_vertex_id', 'INT', 'POINT')
    for i in range(6):
        vertex_ids.data[i].value = i + 1
    uv = mesh.uv_layers.new(name='UVMap')
    records = []
    for i, polygon in enumerate(mesh.polygons):
        face_ids.data[i].value = i + 101
        regions.data[i].value = 9 if i < 3 else 0
        polygon.material_index = 1 if i == 2 else 0
        polygon.use_smooth = i != 3
        for loop in polygon.loop_indices:
            vi = mesh.loops[loop].vertex_index
            uv.data[loop].uv = ((vertices[vi][0] + 1) / 2 + (1 if i == 2 else 0), (vertices[vi][1] + 1) / 2)
    # Persist the authoring representation before making temporary export nodes.
    bpy.ops.wm.save_as_mainfile(filepath=str(out / 'roundtrip.blend'))
    for polygon in mesh.polygons:
        records.append({'id': polygon.index + 101, 'region': 9 if polygon.index < 3 else 0,
                        'material': materials[polygon.material_index].name,
                        'vertices': [v + 1 for v in polygon.vertices],
                        'uv': [list(uv.data[loop].uv) for loop in polygon.loop_indices],
                        'normals': [list(mesh.corner_normals[loop].vector) for loop in polygon.loop_indices],
                        # A continuous displacement field independent of the hard shading edge.
                        'directions': [[0., 0., 1.] for _ in polygon.loop_indices]})
    obj.hide_set(True)
    export_nodes = []
    for selected in (True, False):
        subset = [r for r in records if bool(r['region']) == selected]
        if args.reorder:
            subset.reverse()
        data = bpy.data.meshes.new('ExportCopy')
        # Split corners intentionally; topology is carried by logical IDs, never welding.
        positions = [vertices[v - 1] for r in subset for v in r['vertices']]
        data.from_pydata(positions, [], [(i, i + 1, i + 2) for i in range(0, len(positions), 3)])
        for mat in materials:
            data.materials.append(mat)
        node = bpy.data.objects.new('SilPOM_Selected' if selected else 'SilPOM_Ordinary', data)
        bpy.context.collection.objects.link(node)
        layers = [data.uv_layers.new(name=name) for name in ('UVMap', 'SP_Identity', 'SP_DirectionXY', 'SP_DirectionZ')]
        normal_data = []
        for polygon, record in zip(data.polygons, subset):
            polygon.material_index = 0 if record['material'].endswith('_A') else 1
            polygon.use_smooth = True
            for corner, loop in enumerate(polygon.loop_indices):
                layers[0].data[loop].uv = record['uv'][corner]
                layers[1].data[loop].uv = (record['id'], record['vertices'][corner])
                layers[2].data[loop].uv = record['directions'][corner][:2]
                layers[3].data[loop].uv = (record['directions'][corner][2], 17)
                normal_data.append(record['normals'][corner])
        data.normals_split_custom_set(normal_data)
        export_nodes.append(node)
    bpy.ops.object.select_all(action='DESELECT')
    for node in export_nodes:
        node.select_set(True)
    bpy.context.view_layer.objects.active = export_nodes[0]
    fbx = out / 'roundtrip.fbx'
    bpy.ops.export_scene.fbx(filepath=str(fbx), use_selection=True, object_types={'MESH'},
                             axis_forward='-Y', axis_up='Z', apply_unit_scale=True, apply_scale_options='FBX_SCALE_ALL',
                             use_mesh_modifiers=False, bake_anim=False, add_leaf_bones=False,
                             mesh_smooth_type='OFF', use_custom_props=True)
    document = {'version': 1, 'object_id': obj['silpom_object_id'], 'blender': bpy.app.version_string,
                'fbx_sha256': hashlib.sha256(fbx.read_bytes()).hexdigest(),
                'vertices': {str(i + 1): list(p) for i, p in enumerate(vertices)}, 'faces': records}
    canonical = json.dumps({'vertices': document['vertices'], 'faces': records}, sort_keys=True).encode()
    document['semantic_sha256'] = hashlib.sha256(canonical).hexdigest()
    (out / 'roundtrip.silpom.json').write_text(json.dumps(document, indent=2) + '\n', encoding='utf-8')
    print('SILPOM_EXPORT ' + json.dumps({'blender': bpy.app.version_string, 'faces': len(records), 'fbx': str(fbx)}))


if __name__ == '__main__':
    main()

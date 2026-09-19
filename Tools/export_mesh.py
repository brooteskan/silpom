"""Export tagged static Blender meshes without depending on post-import indices.

blender --background source.blend --python-exit-code 1 --python export_mesh.py --
    --output DIR [--initialize-identities] [--object NAME ...] [--reorder]

The source file is never overwritten. Continue authoring in DIR/authoring.blend,
which contains any newly allocated persistent identities. Output metadata is an
authoring transport contract, NOT the future cooked runtime representation.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import sys
import uuid

import bpy

MAX_TRANSPORT_ID = (1 << 24) - 1


def require(condition, message):
    if not condition:
        raise ValueError(message)


def identity(owner, key, allocated, initialize):
    value = owner.get(key)
    if not isinstance(value, str) or not value or value in allocated:
        require(initialize, f'{owner.name}: missing or duplicated {key}; initialize identities in an authoring copy')
        value = str(uuid.uuid4())
        owner[key] = value
    allocated.add(value)
    return value


def integer_attribute(mesh, name, domain, initialize=False):
    attribute = mesh.attributes.get(name)
    if attribute is None:
        require(initialize, f'{mesh.name}: missing {name}')
        attribute = mesh.attributes.new(name, 'INT', domain)
    require(attribute.data_type == 'INT' and attribute.domain == domain, f'{name}: expected INT/{domain}')
    return attribute


def persistent_ids(mesh, name, domain, initialize):
    attribute = integer_attribute(mesh, name, domain, initialize)
    next_id = max((entry.value for entry in attribute.data), default=0) + 1
    used = set()
    result = []
    for entry in attribute.data:
        if entry.value <= 0 or entry.value in used:
            require(initialize, f'{mesh.name}: invalid or duplicate {name}')
            require(next_id < (1 << 31), f'{name}: identity space exhausted')
            entry.value = next_id
            next_id += 1
        used.add(entry.value)
        result.append(entry.value)
    return result


def unit_direction(value, label):
    require(all(math.isfinite(x) for x in value) and value.length > 1e-12, f'{label}: invalid direction')
    return list(value.normalized())


def export(output, objects, initialize=False, reorder=False):
    require(objects, 'No mesh objects selected for export')
    output.mkdir(parents=True, exist_ok=True)
    source = Path(bpy.data.filepath).resolve() if bpy.data.filepath else None
    authoring = output / 'authoring.blend'
    require(source != authoring.resolve(), 'Choose a new output directory; the input authoring file is never overwritten')
    object_ids, material_ids = set(), set()
    material_map = {}
    vertices, logical_ids, records, object_records = {}, {}, [], []
    unit_scale = bpy.context.scene.unit_settings.scale_length
    require(math.isfinite(unit_scale) and unit_scale > 0, 'Invalid scene unit scale')

    for obj in sorted(objects, key=lambda o: o.name):
        require(obj.type == 'MESH' and obj.library is None and obj.data.library is None, f'{obj.name}: expected local static mesh')
        require(not any(m.show_viewport or m.show_render for m in obj.modifiers), f'{obj.name}: modifiers require explicit baked topology')
        require(obj.data.shape_keys is None, f'{obj.name}: shape keys are unsupported')
        oid = identity(obj, 'silpom_object_id', object_ids, initialize)
        mesh = obj.data
        vertex_ids = persistent_ids(mesh, 'silpom_vertex_id', 'POINT', initialize)
        face_ids = persistent_ids(mesh, 'silpom_face_id', 'FACE', initialize)
        region = mesh.attributes.get('silpom_region_id')
        require(region is not None and region.data_type == 'INT' and region.domain == 'FACE',
                f'{obj.name}: add INT/FACE silpom_region_id (zero means ordinary)')
        profiles = mesh.attributes.get('silpom_profile_id')
        require(profiles is None or (profiles.data_type == 'INT' and profiles.domain == 'FACE'), 'Invalid profile attribute')
        directions = mesh.attributes.get('silpom_direction')
        require(directions is None or (directions.data_type == 'FLOAT_VECTOR' and directions.domain in ('POINT', 'CORNER')),
                'silpom_direction must be FLOAT_VECTOR/POINT or CORNER')
        require(mesh.uv_layers.active is not None, f'{obj.name}: missing authored UVs')
        uv_layer = mesh.uv_layers.active
        transform = obj.matrix_world.copy()
        linear = transform.to_3x3()
        columns = [linear.col[i].copy() for i in range(3)]
        scales = [v.length for v in columns]
        require(min(scales) > 1e-12 and max(scales) - min(scales) <= 1e-5 * max(scales) and linear.determinant() > 0,
                f'{obj.name}: only positive uniform scale is supported')
        require(all(abs(columns[i].dot(columns[j])) <= 1e-5 * scales[i] * scales[j]
                    for i in range(3) for j in range(i)), f'{obj.name}: shear is unsupported')
        vertex_transport = {}
        for vertex, vid in zip(mesh.vertices, vertex_ids):
            key = len(vertices) + 1
            require(key <= MAX_TRANSPORT_ID, 'Too many logical vertices for exact transport')
            position = (transform @ vertex.co) * unit_scale
            require(all(math.isfinite(x) for x in position), 'Non-finite position')
            vertices[str(key)] = list(position)
            logical_ids[str(key)] = f'{oid}:{vid}'
            vertex_transport[vertex.index] = key
        mesh.calc_loop_triangles()
        for triangle in mesh.loop_triangles:
            polygon = mesh.polygons[triangle.polygon_index]
            rid = region.data[polygon.index].value
            profile = profiles.data[polygon.index].value if profiles else (1 if rid else 0)
            require(rid >= 0 and profile >= 0 and (rid == 0 or profile > 0), 'Invalid region/profile assignment')
            require(polygon.material_index < len(mesh.materials) and mesh.materials[polygon.material_index] is not None,
                    f'{obj.name}: every face needs a material')
            material = mesh.materials[polygon.material_index]
            if material.name not in material_map:
                mid = identity(material, 'silpom_material_id', material_ids, initialize)
                material_map[material.name] = {'id': mid, 'name': material.name, 'export_name': 'SPM_' + hashlib.sha256(mid.encode()).hexdigest()[:24]}
            mat = material_map[material.name]
            local_ids = [vertex_ids[index] for index in triangle.vertices]
            # Rotation-independent oriented triangle key survives export order.
            canonical = min(tuple(local_ids[i:] + local_ids[:i]) for i in range(3))
            stable = f'{oid}:{face_ids[polygon.index]}:' + ','.join(map(str, canonical))
            corner_uv, normals, field = [], [], []
            for loop in triangle.loops:
                value = list(uv_layer.data[loop].uv)
                require(all(math.isfinite(x) for x in value), 'Non-finite UV')
                corner_uv.append(value)
                normal = mesh.corner_normals[loop].vector
                normals.append(unit_direction(linear @ normal, 'shading normal'))
                if directions:
                    index = loop if directions.domain == 'CORNER' else mesh.loops[loop].vertex_index
                    vector = directions.data[index].vector
                else:
                    vector = normal
                field.append(unit_direction(linear @ vector, 'displacement'))
            records.append({'stable_id': stable, 'object_id': oid, 'region': rid, 'profile': profile,
                            'material': mat['export_name'], 'material_id': mat['id'],
                            'vertices': [vertex_transport[i] for i in triangle.vertices],
                            'uv': corner_uv, 'normals': normals, 'directions': field})
        object_records.append({'id': oid, 'name': obj.name})

    require(records and len(records) <= MAX_TRANSPORT_ID, 'Invalid triangle count for exact transport')
    require(len({r['stable_id'] for r in records}) == len(records), 'Ambiguous triangulation identity')
    records.sort(key=lambda r: r['stable_id'])
    for index, record in enumerate(records, 1):
        record['id'] = index
    # Save persistent IDs before creating temporary export meshes/materials.
    bpy.ops.wm.save_as_mainfile(filepath=str(authoring), copy=True)
    material_records = sorted(material_map.values(), key=lambda m: m['id'])
    export_materials = []
    for record in material_records:
        material = bpy.data.materials[record['name']].copy()
        material.name = record['export_name']
        require(material.name == record['export_name'], 'Reserved export material name already exists')
        export_materials.append(material)
    material_slots = {m['id']: i for i, m in enumerate(material_records)}
    nodes = []
    for selected in (True, False):
        subset = [r for r in records if bool(r['region']) == selected]
        if not subset:
            continue
        if reorder:
            subset.reverse()
        name = 'SilPOM_Selected' if selected else 'SilPOM_Ordinary'
        require(name not in bpy.data.objects, 'Reserved export node name already exists: ' + name)
        mesh = bpy.data.meshes.new(name)
        positions = [vertices[str(v)] for record in subset for v in record['vertices']]
        mesh.from_pydata(positions, [], [(i, i + 1, i + 2) for i in range(0, len(positions), 3)])
        for material in export_materials:
            mesh.materials.append(material)
        node = bpy.data.objects.new(name, mesh)
        bpy.context.collection.objects.link(node)
        layers = [mesh.uv_layers.new(name=n) for n in ('UVMap', 'SP_Identity', 'SP_DirectionXY', 'SP_DirectionZ')]
        shading = []
        for polygon, record in zip(mesh.polygons, subset):
            polygon.material_index = material_slots[record['material_id']]
            polygon.use_smooth = True
            for corner, loop in enumerate(polygon.loop_indices):
                layers[0].data[loop].uv = record['uv'][corner]
                layers[1].data[loop].uv = (record['id'], record['vertices'][corner])
                layers[2].data[loop].uv = record['directions'][corner][:2]
                layers[3].data[loop].uv = (record['directions'][corner][2], 17)
                shading.append(record['normals'][corner])
        mesh.normals_split_custom_set(shading)
        nodes.append(node)
    bpy.ops.object.select_all(action='DESELECT')
    for node in nodes:
        node.select_set(True)
    bpy.context.view_layer.objects.active = nodes[0]
    bpy.context.scene.unit_settings.scale_length = 1.0  # positions were already resolved to metres
    fbx = output / 'roundtrip.fbx'
    bpy.ops.export_scene.fbx(filepath=str(fbx), use_selection=True, object_types={'MESH'}, axis_forward='-Y', axis_up='Z',
                            apply_unit_scale=True, apply_scale_options='FBX_SCALE_ALL', use_mesh_modifiers=False,
                            bake_anim=False, add_leaf_bones=False, mesh_smooth_type='OFF', use_custom_props=True)
    semantics = {'vertices': vertices, 'logical_vertex_ids': logical_ids, 'faces': records,
                 'objects': object_records, 'materials': material_records}
    document = {'version': 2, 'blender': bpy.app.version_string,
                'fbx_sha256': hashlib.sha256(fbx.read_bytes()).hexdigest(), **semantics,
                'semantic_sha256': hashlib.sha256(json.dumps(semantics, sort_keys=True).encode()).hexdigest()}
    (output / 'roundtrip.silpom.json').write_text(json.dumps(document, indent=2) + '\n', encoding='utf-8')
    print('SILPOM_EXPORT ' + json.dumps({'faces': len(records), 'objects': len(objects), 'output': str(output)}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--object', action='append', dest='objects')
    parser.add_argument('--initialize-identities', action='store_true')
    parser.add_argument('--reorder', action='store_true')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    objects = [bpy.data.objects[name] for name in args.objects] if args.objects else [o for o in bpy.context.scene.objects if o.type == 'MESH']
    export(args.output.resolve(), objects, args.initialize_identities, args.reorder)

"""Validate an actual O3DE imported graph against a hash-bound FBX sidecar."""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def close(actual, expected, label, tolerance=2e-5):
    require(len(actual) == len(expected) and all(math.isfinite(a) and abs(a-b) <= tolerance
                                                for a, b in zip(actual, expected)), f'{label}: {actual} != {expected}')


def integer(value):
    require(math.isfinite(value) and abs(value-round(value)) < 1e-5, 'corrupt noninteger identity')
    return round(value)


def validate(sidecar, imported, fbx_bytes):
    require(sidecar['version'] in (1, 2), 'unsupported sidecar version')
    require(hashlib.sha256(fbx_bytes).hexdigest() == sidecar['fbx_sha256'], 'FBX/sidecar generation mismatch')
    require(imported.get('fbx_sha256') == sidecar['fbx_sha256'], 'stale importer report')
    require(not imported.get('error'), str(imported.get('error')))
    semantic_keys = ('vertices', 'faces') if sidecar['version'] == 1 else ('vertices', 'logical_vertex_ids', 'faces', 'objects', 'materials')
    canonical = json.dumps({key: sidecar[key] for key in semantic_keys}, sort_keys=True).encode()
    require(hashlib.sha256(canonical).hexdigest() == sidecar['semantic_sha256'], 'corrupt semantic signature')
    expected = {f['id']: f for f in sidecar['faces']}
    require(len(expected) == len(sidecar['faces']), 'duplicate authored face ID')
    if sidecar['version'] == 2:
        require(len({f['stable_id'] for f in expected.values()}) == len(expected), 'duplicate stable face identity')
        logical_ids = sidecar['logical_vertex_ids']
        require(set(logical_ids) == set(sidecar['vertices']) and len(set(logical_ids.values())) == len(logical_ids),
                'ambiguous logical vertex identity')
        materials = {m['id']: m for m in sidecar['materials']}
        require(len(materials) == len(sidecar['materials']), 'duplicate material identity')
        objects = {o['id'] for o in sidecar['objects']}
        require(len(objects) == len(sidecar['objects']), 'duplicate object identity')
        require(all(isinstance(value, str) and value for value in logical_ids.values()), 'empty logical identity')
        for f in expected.values():
            require(f['object_id'] in objects and f['stable_id'].startswith(f['object_id'] + ':'), 'unknown face object')
            require(all(str(vid) in logical_ids and logical_ids[str(vid)].startswith(f['object_id'] + ':')
                        for vid in f['vertices']), 'vertex object namespace mismatch')
            require(f['material_id'] in materials and materials[f['material_id']]['export_name'] == f['material'],
                    'material mapping disagrees with authored identity')
            require(f['region'] >= 0 and f['profile'] >= 0 and (not f['region'] or f['profile'] > 0), 'invalid profile')
            require(0 < f['id'] < (1 << 24) and len(f['vertices']) == 3, 'invalid face transport ID')
            require(all(0 < vid < (1 << 24) for vid in f['vertices']), 'invalid vertex transport ID')
            require(all(len(d) == 3 and all(math.isfinite(x) for x in d) and abs(sum(x*x for x in d)-1) < 2e-5
                        for d in f['directions']), 'invalid displacement direction')
    seen = {}
    nodes = set()
    for mesh in imported['meshes']:
        # AssImp preserves names on single-material nodes but this supported
        # importer uses UV0..UV3 when a node spans multiple material meshes.
        names = ('UVMap', 'SP_Identity', 'SP_DirectionXY', 'SP_DirectionZ')
        uv = {name: mesh['uv'].get(name, mesh['uv'].get('UV' + str(i))) for i, name in enumerate(names)}
        require(all(uv.values()), 'missing transport UV channel')
        for face in mesh['faces']:
            indices = face['indices']
            ids = [integer(uv['SP_Identity'][i][0]) for i in indices]
            require(len(set(ids)) == 1 and ids[0] in expected, 'unknown/mixed face identity')
            fid = ids[0]
            require(fid not in seen, 'duplicate imported face identity')
            record = expected[fid]
            logical = [integer(1-uv['SP_Identity'][i][1]) for i in indices]
            require(set(logical) == set(record['vertices']) and len(set(logical)) == 3, 'logical topology changed')
            authored = record['vertices']
            require(any(logical == authored[k:] + authored[:k] for k in range(3)), 'triangle winding changed')
            require(('SilPOM_Selected' in mesh['path']) == bool(record['region']), 'selection partition changed')
            material = mesh['materials'][face['material']]
            require(material == record['material'], 'material identity changed: ' + material)
            for i, vid in zip(indices, logical):
                corner = record['vertices'].index(vid)
                close(mesh['positions'][i], sidecar['vertices'][str(vid)], 'position/unit/axis change')
                close([uv['UVMap'][i][0], 1-uv['UVMap'][i][1]], record['uv'][corner], 'UV changed')
                # Blender's custom split-normal storage quantizes direction. Keep
                # this separate from the UV/position transport error budget.
                close(mesh['normals'][i], record['normals'][corner], 'shading normal changed', tolerance=2e-4)
                close([uv['SP_DirectionXY'][i][0], 1-uv['SP_DirectionXY'][i][1], uv['SP_DirectionZ'][i][0]],
                      record['directions'][corner], 'displacement direction changed')
                close([1-uv['SP_DirectionZ'][i][1]], [17], 'transport sentinel corrupted')
            seen[fid] = logical
            nodes.add(mesh['path'])
    require(set(seen) == set(expected), 'missing faces')
    edges = {}
    for fid, vertices in seen.items():
        for i in range(3):
            edge = tuple(sorted((vertices[i], vertices[(i+1) % 3])))
            edges.setdefault(edge, []).append(fid)
    shared = [v for v in edges.values() if len(v) == 2]
    cross_partition = [v for v in shared if bool(expected[v[0]]['region']) != bool(expected[v[1]]['region'])]
    authored_edges = {}
    for fid, record in expected.items():
        for i in range(3):
            edge = tuple(sorted((record['vertices'][i], record['vertices'][(i+1) % 3])))
            authored_edges.setdefault(edge, []).append(fid)
    require({edge: sorted(faces) for edge, faces in edges.items()} ==
            {edge: sorted(faces) for edge, faces in authored_edges.items()}, 'adjacency across partition lost')
    require(all(len(faces) <= 2 for faces in edges.values()), 'nonmanifold edge is unsupported')
    return {'passed': True, 'faces': len(seen), 'selected': sum(bool(f['region']) for f in expected.values()),
            'materials': sorted(set(f['material'] for f in expected.values())), 'nodes': sorted(nodes),
            'shared_edges': len(shared), 'selected_ordinary_edges': len(cross_partition),
            'semantic_sha256': sidecar['semantic_sha256'], 'blender': sidecar['blender']}


def negative_tests(sidecar, imported, fbx):
    def reject(s, i, b, label):
        try:
            validate(s, i, b)
        except (ValueError, KeyError, IndexError):
            return
        raise AssertionError('accepted ' + label)
    reject(sidecar, imported, fbx + b'bad', 'stale sidecar')
    changed = copy.deepcopy(imported)
    channels = changed['meshes'][0]['uv']
    channels.get('SP_Identity', channels.get('UV1'))[0][0] += .25
    reject(sidecar, changed, fbx, 'corrupted transport')
    changed = copy.deepcopy(imported)
    changed['meshes'][0]['faces'] *= 2
    reject(sidecar, changed, fbx, 'duplicate object/face identity')
    changed = copy.deepcopy(imported)
    changed['meshes'][0]['materials'] = ['wrong-material'] * len(changed['meshes'][0]['materials'])
    reject(sidecar, changed, fbx, 'wrong material')
    reject({}, imported, fbx, 'missing sidecar')
    return 5


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    sidecar = json.loads((args.directory / 'roundtrip.silpom.json').read_text())
    imported = json.loads((args.directory / 'roundtrip.imported.json').read_text())
    fbx = (args.directory / 'roundtrip.fbx').read_bytes()
    result = validate(sidecar, imported, fbx)
    result['negative_checks'] = negative_tests(sidecar, imported, fbx)
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))

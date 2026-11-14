"""Install the frozen tower benchmark, or explicitly regenerate it from TG assets."""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path

NAME = 'SilPOMTowerBenchmark'
FIXTURE = Path(__file__).resolve().parents[1] / 'Tests/Fixtures/TowerBenchmark'
TOWER = 'Assets/SilPOMPlanarAB/IrishFieldstoneTowerOpenShaft/IrishFieldstoneTowerOpenShaft.prefab'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def patched(source, patches):
    """Resolve the saved instance for inspection; preserve its patches in the level."""
    value = copy.deepcopy(source)
    for entity in [value['ContainerEntity'], *value.get('Entities', {}).values()]:
        for component in entity.get('Components', {}).values():
            if 'TransformComponent' in component.get('$type', ''):
                data = component.setdefault('Transform Data', {})
                data.setdefault('Translate', [0, 0, 0])
                data.setdefault('Rotate', [0, 0, 0])
                data.setdefault('Scale', 1)
    for patch in patches:
        parts = [p.replace('~1', '/').replace('~0', '~') for p in patch['path'].split('/')[1:]]
        parent = value
        for part in parts[:-1]:
            parent = parent[int(part)] if isinstance(parent, list) else parent[part]
        key = int(parts[-1]) if isinstance(parent, list) and parts[-1] != '-' else parts[-1]
        if patch['op'] == 'remove':
            if isinstance(parent, list):
                del parent[key]
            else:
                parent.pop(key, None)  # Prefab overrides can retain stale removals.
        elif patch['op'] in ('add', 'replace'):
            item = copy.deepcopy(patch['value'])
            if isinstance(parent, list) and patch['op'] == 'add':
                parent.append(item) if key == '-' else parent.insert(key, item)
            else:
                parent[key] = item
        else:
            raise ValueError('Unsupported prefab patch: ' + patch['op'])
    return value


def transform(entity, root, position, rotation, scale=1):
    component = next(c for c in entity['Components'].values() if 'TransformComponent' in c['$type'])
    component['Parent Entity'] = root
    component['Transform Data'] = {'Translate': list(position), 'Rotate': list(rotation), 'Scale': scale}


def angles(direction):
    return [math.degrees(math.atan2(direction[2], math.hypot(*direction[:2]))), 0.0,
            (math.degrees(math.atan2(direction[1], direction[0])) + 90) % 360 - 180]


def generate(project):
    original = (project / 'Levels/VueLiveLink/VueLiveLink.prefab').read_bytes()
    live = json.loads(original)
    defaults = json.loads((project / 'user/engine-source/Assets/Editor/Prefabs/Default_Level.prefab').read_bytes())
    source = json.loads((project / TOWER).read_bytes())
    key, instance = next((k, copy.deepcopy(v)) for k, v in live['Instances'].items() if v['Source'] == TOWER)
    inspect = [p for p in instance['Patches'] if p['path'].startswith('/Entities/TowerMesh/Components/EditorMeshComponent/')
               or (p['op'] == 'remove' and p['path'].startswith('/Entities/') and p['path'].count('/') == 2)]
    resolved = patched(source, inspect)
    config = resolved['Entities']['TowerMesh']['Components']['EditorMeshComponent']['Controller']['Configuration']
    # Keep the tower geometry and its complete shader configuration. The new
    # lighting fixture has no baked GI dependencies. Keep authored window
    # lights so interior edge comparisons remain visibly illuminated.
    removed = []
    for alias, entity in resolved['Entities'].items():
        is_light = any('AreaLightComponent' in c['$type'] for c in entity['Components'].values())
        if alias not in ('TowerMesh', 'UpperRoofLining') and not is_light:
            instance['Patches'].append({'op': 'remove', 'path': '/Entities/' + alias})
            removed.append(entity['Name'])
    instance['Patches'].append({'op': 'replace',
        'path': '/Entities/TowerMesh/Components/EditorEntitySortComponent/Child Entity Order',
        'value': [alias for alias in resolved['Entities'] if alias not in ('TowerMesh',) and
            (alias == 'UpperRoofLining' or any('AreaLightComponent' in c['$type']
                for c in resolved['Entities'][alias]['Components'].values()))]})
    instance['Patches'].append({'op': 'replace',
        'path': '/ContainerEntity/Components/TransformComponent/Transform Data',
        'value': {'Translate': [0, 0, 0], 'Rotate': [0, 0, 0], 'Scale': 1}})
    root = defaults['ContainerEntity']
    root['Name'] = NAME
    root_id = root['Id']
    entities = {}
    for entity in defaults['Entities'].values():
        if entity['Name'] not in ('Sun', 'Camera', 'Global Sky', 'Ground'):
            continue
        entity = copy.deepcopy(entity)
        transform(entity, root_id, [0, 0, 0], [0, 0, 0])
        entities[entity['Id']] = entity
    sun = next(e for e in entities.values() if e['Name'] == 'Sun')
    transform(sun, root_id, [0, 0, 30], [-60, 0, -35])
    sky = next(e for e in entities.values() if e['Name'] == 'Global Sky')
    sky['Name'] = 'Sky and Environment | Daylight IBL'
    ground = next(e for e in entities.values() if e['Name'] == 'Ground')
    transform(ground, root_id, [0, 0, -.03], [0, 0, 0], .125)
    mesh = json.loads((project / Path(TOWER).parent / 'tower-open-shaft-no-landings-silpom.silpom.json').read_bytes())
    points = list(mesh['vertices'].values())
    low, high = min(v[2] for v in points), max(v[2] for v in points)
    distance = (high-low) / (2*math.tan(math.radians(55/2))) * 1.2
    views = {'exterior': {'position': [.6*distance, -.8*distance, (high+low)/2],
                          'rotation': angles([-.6*distance, .8*distance, 0])}}
    material_ids = {m['id'] for m in mesh['materials'] if 'sculpt relief' in m['name']}
    treads = []
    for face in mesh['faces']:
        if face['material_id'] not in material_ids:
            continue
        a, b, c = [mesh['vertices'][str(i)] for i in face['vertices']]
        point = [(x+y+z)/3 for x, y, z in zip(a, b, c)]
        if max(a[2], b[2], c[2])-min(a[2], b[2], c[2]) <= .07 and 1.36 < math.hypot(*point[:2]) < 1.55:
            treads.append(point)
    for prefix, height in (('lower', 8.39), ('upper', 16.59)):
        x, y, z = min(treads, key=lambda t: abs(t[2]-height))
        azimuth = math.atan2(y, x)
        tangent = azimuth+math.pi-math.asin(.905/math.hypot(x, y))
        views[prefix+'-glancing'] = {'position': [x, y, z+1.58],
            'rotation': angles([math.cos(tangent), math.sin(tangent), .16])}
        if prefix == 'lower':
            views['lower-facing'] = {'position': [x, y, z+1.58],
                'rotation': angles([math.cos(azimuth), math.sin(azimuth), -.04])}
    hero = next(e for e in entities.values() if e['Name'] == 'Camera')
    hero['Name'] = 'Camera | Exterior (Fly Camera)'
    transform(hero, root_id, **views['exterior'])
    for index, (name, view) in enumerate(views.items()):
        if name == 'exterior':
            continue
        camera = copy.deepcopy(hero)
        camera['Id'] = f'Entity_[910570000000{index}]'
        camera['Name'] = 'Camera | ' + name
        camera['Components'] = {k: c for k, c in camera['Components'].items()
                                if c['$type'] != 'GenericComponentWrapper'}
        next(c for c in camera['Components'].values() if c['$type'] == 'EditorOnlyEntityComponent')['IsEditorOnly'] = True
        transform(camera, root_id, **view)
        entities[camera['Id']] = camera
    for index, prefix in enumerate(('lower', 'upper')):
        light = copy.deepcopy(source['Entities']['UpperWestWindowDaylight'])
        light['Id'] = f'Entity_[910570000010{index}]'
        light['Name'] = 'Comparison Fill | ' + prefix
        x, y, z = views[prefix+'-glancing']['position']
        transform(light, root_id, [x, y, z], [90, 0, math.degrees(math.atan2(-x, y))])
        lighting = next(c['Controller']['Configuration'] for c in light['Components'].values()
                        if 'AreaLightComponent' in c['$type'])
        lighting.update(Intensity=30.0, EnableShutters=False, LightEmitsBothDirections=True,
                        UseFastApproximation=True, AttenuationRadius=4.0)
        lighting['Enable Shadow'] = False
        lighting['LightingChannelConfig'] = {'lightingChannelFlags': [True]*5}
        entities[light['Id']] = light
    for c in root['Components'].values():
        if c['$type'] == 'EditorEntitySortComponent':
            c['Child Entity Order'] = list(entities) + [key + '/ContainerEntity']
    defaults.update(ContainerEntity=root, Entities=entities, Instances={key: instance})
    data = (json.dumps(defaults, indent=4)+'\n').encode()
    manifest = {'schema': 1, 'name': NAME, 'scene_sha256': digest(data),
        'source_level_sha256': digest(original), 'source_tower_prefab': TOWER,
        'tower_configuration': config, 'tower_configuration_sha256': digest(json.dumps(config, sort_keys=True).encode()),
        'anti_aliasing': config.get('AntiAliasing', 'CurrentDefault'),
        'views': views, 'removed_from_benchmark_only': removed,
        'required_source_trees': ['Assets/SilPOMPlanarAB/IrishFieldstoneTowerOpenShaft',
            'Assets/SilPOMPlanarAB/IrishFieldstoneTowerWelded', 'Assets/SilPOMLearned'],
        'lighting': ['Sun', 'Sky and Environment | Daylight IBL', 'Comparison Fill | lower', 'Comparison Fill | upper'],
        'recommended_capture_resolution': [1280, 720], 'target_system': 'Ryzen 7 5700G'}
    assert (project / 'Levels/VueLiveLink/VueLiveLink.prefab').read_bytes() == original
    FIXTURE.mkdir(parents=True, exist_ok=True)
    (FIXTURE / (NAME+'.prefab')).write_bytes(data)
    (FIXTURE / 'scene.json').write_text(json.dumps(manifest, indent=2)+'\n')


def install(project, replace=False):
    folder = project / 'Levels' / NAME
    folder.mkdir(parents=True, exist_ok=True)
    for name in (NAME+'.prefab', 'scene.json'):
        data = (FIXTURE / name).read_bytes()
        target = folder / name
        if target.exists() and target.read_bytes() != data and not replace:
            raise ValueError(f'{target} differs; use --replace only for an intentional fixture update')
        target.write_bytes(data)
    print(folder / (NAME+'.prefab'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, required=True)
    parser.add_argument('--generate', action='store_true', help='Regenerate the frozen scene from this project\'s saved VueLiveLink tower')
    parser.add_argument('--replace', action='store_true')
    args = parser.parse_args()
    if args.generate:
        generate(args.project.resolve())
    install(args.project.resolve(), args.replace)

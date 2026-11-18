"""Capture explicit per-winning-sample gradients on the fixed 2x tower fixture.

Run in a dedicated Editor --runpython with processed assets and
SILPOM_GRADIENT_OUTPUT set to a fresh directory. This script never
collects timestamps. Run test_tower_gradient_context.py on its output afterward.
The saved level is unchanged and the temporary normals/AA/view settings are
restored. The four fixed tower views match TowerResolveRegionsBenchmark.py. A final
wider representation probe is enabled by default; set
SILPOM_GRADIENT_DIAGNOSTIC=0 to omit it. No timestamps are collected.
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import math
import sqlite3
import struct
import time
import traceback
import uuid

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import silpom_editor_paths as silpom_paths
import azlmbr.atom as atom
import azlmbr.asset as asset
import azlmbr.bus as bus
import azlmbr.components as components
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.math as azmath
import azlmbr.paths as paths
import azlmbr.silpom as silpom

PROJECT = Path(paths.projectroot)
LEVEL = PROJECT / 'Levels/SilPOMTowerBenchmark/SilPOMTowerBenchmark.prefab'
OUTPUT = Path(os.environ['SILPOM_GRADIENT_OUTPUT'])
assert not (OUTPUT / 'results.json').exists(), 'Choose a fresh capture output directory'
OUTPUT.mkdir(parents=True, exist_ok=True)
VIEWS = ('exterior', 'lower-facing', 'lower-glancing', 'upper-glancing')
if os.environ.get('SILPOM_GRADIENT_VIEWS'):
    requested = tuple(name.strip() for name in os.environ['SILPOM_GRADIENT_VIEWS'].split(','))
    assert requested and len(set(requested)) == len(requested) and all(name in VIEWS for name in requested)
    VIEWS = requested
SAMPLES = 2
VIEWPORT = (1280, 720)
result = {'passed': False, 'stage': 'startup', 'runs': [],
          'benchmark_performed': False, 'samples': SAMPLES, 'viewport': list(VIEWPORT),
          'validation_scope': 'focused-pilot' if os.environ.get('SILPOM_GRADIENT_VIEWS') else 'full-view-set',
          'gradient_contract': 'owner-local-first-order-per-raster-pixel',
          'normal_query_policy': 'prepared-normal-stencil-with-probe-validity',
          'face_cache_policy': 'generation-coefficients-frame-local-implicit-near-v1',
          'face_cache_enabled': os.environ.get('SILPOM_FACE_CACHE', '1') != '0',
          'face_cache_validation_sequence': 'off/on/off' if os.environ.get('SILPOM_FACE_CACHE_COMPARE', '0') == '1' else 'fixed',
          'attachment_suffixes': ['hit', 'depth', 'diffuse', 'specular', 'normal', 'uv-gradients', 'depth-gradients'],
          'paint_probe_enabled': os.environ.get('SILPOM_GRADIENT_PAINT_PROBE', '1') != '0'}
cvars = {}
initial_view = {}
component = None
normals_path = None
original_normals = None
tower = None
property_restores = {}
paint_canvas = None
PAINT_NAME = 'Gradient Context Paint Probe'


def flush():
    (OUTPUT / 'results.json').write_text(json.dumps(result, indent=2), encoding='utf-8')


def command(name, value):
    if name not in cvars:
        cvars[name] = general.get_cvar(name)
    general.run_console(f'{name} {value}')


def read(path):
    response = editor.EditorComponentAPIBus(bus.Broadcast, 'GetComponentProperty', component, path)
    assert response.IsSuccess(), path
    return response.GetValue()


def write(path, value):
    response = editor.EditorComponentAPIBus(bus.Broadcast, 'SetComponentProperty', component, path, value)
    assert response.IsSuccess(), (path, str(response.GetError()))
    assert read(path) == value, path


def temporary_property(properties, label, value):
    path = next(p for p in properties if p.endswith('|' + label))
    if path not in property_restores:
        property_restores[path] = read(path)
    write(path, value)


def restore_properties():
    # Reverse order restores the 5A prerequisite before any original 5B setting.
    for path, value in reversed(tuple(property_restores.items())):
        write(path, value)
    property_restores.clear()


def aa_state():
    state = silpom.SilPomMeshRequestBus(bus.Event, 'GetAntiAliasingState', tower)
    return [int(state.x), int(state.y), int(state.z)]


def ready(expected_aa=None):
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        general.idle_wait_frames(1)
        if not silpom.SilPomMeshRequestBus(bus.Event, 'IsReady', tower):
            continue
        if expected_aa is None or aa_state() == expected_aa:
            general.idle_wait_frames(30)
            return
    raise AssertionError(str(silpom.SilPomMeshRequestBus(bus.Event, 'GetStatus', tower)))


def renderer_clean():
    log = (PROJECT / 'user/log/Editor.log').read_text(errors='replace')
    errors = ('Failed to bind shader resources for draw item',
              '[Error] (DeviceShaderResourceGroupData)', 'Trace::Assert')
    assert not any(error in log for error in errors), 'Renderer binding/assert error; inspect Editor.log'


def capture(path, slot, hierarchy):
    done = []
    handler = atom.FrameCaptureNotificationBusHandler()
    response = atom.FrameCaptureRequestBus(bus.Broadcast, 'CapturePassAttachment', str(path), hierarchy, slot, 1)
    assert response.IsSuccess(), (slot, str(response.GetError()))
    handler.connect(response.GetValue())
    handler.add_callback('OnFrameCaptureFinished', lambda args: done.append(args[0]))
    try:
        deadline = time.monotonic() + 60
        while not done and time.monotonic() < deadline:
            general.idle_wait_frames(1)
        assert done == [atom.FrameCaptureResult_Success], (slot, done)
    finally:
        handler.disconnect()


def snapshot(stem, selected=None):
    records = ['Root', 'MainPipeline_0', 'DepthPrePass', 'SilPOMInspectFinal']
    colors = ['Root', 'MainPipeline_0', 'OpaquePass', 'SilPOMInspectColors']
    for suffix, slot, hierarchy in (
        ('hit', 'Records', records), ('depth', 'SampleDepth', records),
        ('diffuse', 'DiffuseSamples', colors), ('specular', 'SpecularSamples', colors),
        ('normal', 'NormalSamples', colors),
        ('uv-gradients', 'SampleUvGradients', records),
        ('depth-gradients', 'SampleDepthGradients', records),
    ):
        if selected is not None and suffix not in selected:
            continue
        capture(OUTPUT / f'{stem}-{suffix}.dds', slot, hierarchy)


def paint_document(document):
    error = silpom.DecalPaintRequestBus(bus.Event, 'SetDocumentJson', paint_canvas, json.dumps(
        {'Type': 'JsonSerialization', 'Version': 1, 'ClassName': 'PaintConfig', 'ClassData': document}))
    assert not error, error


def paint_state():
    return {'receiver': json.loads(silpom.SilPomMeshRequestBus(bus.Event, 'GetReceiverUvPaintStatus', tower)),
            'paint': str(silpom.DecalPaintRequestBus(bus.Event, 'GetPaintStatus', paint_canvas)),
            'fixed_dabs': int(silpom.DecalPaintRequestBus(bus.Event, 'GetCompositeDabCount', paint_canvas)),
            'live_dabs': int(silpom.DecalPaintRequestBus(bus.Event, 'GetDabCount', paint_canvas)),
            'render_stats': json.loads(silpom.DecalPaintRequestBus(bus.Event, 'GetRenderStatsJson', paint_canvas))}


def prepare_paint_probe():
    """Two fixed and two live dabs on a face proven visible in this exact view."""
    global paint_canvas
    probe = result['paint_probe'] = {'passed': False, 'resolution': 64,
                                     'pristine_stem': '2x-painted-lower-facing-pristine'}
    command('r_silpomResolveRegions', 0)
    general.idle_wait_frames(30)
    snapshot(probe['pristine_stem'], ('hit', 'depth', 'diffuse', 'normal'))
    faces = {int(face['hit_face']): face for face in json.loads(
        silpom.SilPomMeshRequestBus(bus.Event, 'GetPaintFacesJson', tower))}
    raw = (OUTPUT / (probe['pristine_stem'] + '-hit.dds')).read_bytes()
    assert raw[:4] == b'DDS ' and raw[84:88] == b'DX10'
    assert struct.unpack_from('<II', raw, 12) == (VIEWPORT[1], VIEWPORT[0] * 4)
    assert struct.unpack_from('<I', raw, 128)[0] == 3

    def sub(a, b): return tuple(x - y for x, y in zip(a, b))
    def length(a): return math.sqrt(sum(x * x for x in a))
    candidates = [(x, y) for y in range(VIEWPORT[1] // 4, VIEWPORT[1] * 3 // 4, 24)
                  for x in range(VIEWPORT[0] // 4, VIEWPORT[0] * 3 // 4, 24)]
    candidates.sort(key=lambda point: (point[0] - VIEWPORT[0] / 2) ** 2 + (point[1] - VIEWPORT[1] / 2) ** 2)
    selected = None
    for x, y in candidates:
        for sample in range(SAMPLES):
            offset = 148 + ((y * VIEWPORT[0] * 4 + x * 4 + sample) * 16)
            record = struct.unpack_from('<4I', raw, offset)
            face = faces.get(record[3] >> 2)
            if record[3] & 3 != 1 or face is None or face['profile'] != 1:
                continue
            uv = struct.unpack_from('<2f', raw, offset)
            uvs = [corner['uv'] for corner in face['corners']]
            a, b = sub(uvs[1], uvs[0]), sub(uvs[2], uvs[0])
            determinant = a[0] * b[1] - a[1] * b[0]
            if abs(determinant) < 1e-12:
                continue
            def weights(q):
                c = sub(q, uvs[0])
                v = (c[0] * b[1] - c[1] * b[0]) / determinant
                w = (a[0] * c[1] - a[1] * c[0]) / determinant
                return (1 - v - w, v, w)
            if min(weights(uv)) < .15:
                continue
            positions = [corner['position'] for corner in face['corners']]
            e1, e2 = sub(positions[1], positions[0]), sub(positions[2], positions[0])
            du = tuple((e1[k] * b[1] - e2[k] * a[1]) / determinant for k in range(3))
            dv = tuple((e2[k] * a[0] - e1[k] * b[0]) / determinant for k in range(3))
            if min(length(du), length(dv)) < 1e-6:
                continue
            ru, rv = .10 / length(du), .10 / length(dv)
            for _ in range(24):
                if min(min(weights((uv[0] + u * ru, uv[1] + v * rv))) for u in (-1, 1) for v in (-1, 1)) >= .05:
                    break
                ru *= .8; rv *= .8
            if min(ru * length(du), rv * length(dv)) >= .025:
                normal = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                          e1[0] * e2[1] - e1[1] * e2[0])
                eye = general.get_current_view_position()
                front = sum(normal[k] * ((eye.x, eye.y, eye.z)[k] - positions[0][k]) for k in range(3)) > 0
                selected = (face, uv, ru, rv, x, y, sample, front)
                break
        if selected is not None:
            break
    assert selected is not None, 'No well-contained visible wall chart for the bounded paint probe'
    face, uv, ru, rv, x, y, sample, front = selected
    rectangle = (uv[0] - ru, uv[1] - rv, uv[0] + ru, uv[1] + rv)
    # Same unsaved entity/component route as TowerBenchmarkPaintGroupingSmoke.
    paint_canvas = editor.ToolsApplicationRequestBus(bus.Broadcast, 'CreateNewEntity', entity.EntityId())
    assert paint_canvas.IsValid()
    editor.EditorEntityAPIBus(bus.Event, 'SetName', paint_canvas, PAINT_NAME)
    kinds = editor.EditorComponentAPIBus(bus.Broadcast, 'FindComponentTypeIdsByEntityType',
                                       ['Surface Decal Paint'], entity.EntityType().Game)
    assert len(kinds) == 1
    added = editor.EditorComponentAPIBus(bus.Broadcast, 'AddComponentsOfType', paint_canvas, kinds)
    assert added.IsSuccess(), str(added.GetError())
    components.TransformBus(bus.Event, 'SetWorldTM', paint_canvas, azmath.Transform_CreateIdentity())
    path = 'materials/decalpaint/hard_round.azmaterial'
    material_id = asset.AssetCatalogRequestBus(bus.Broadcast, 'GetAssetIdByPath', path, azmath.Uuid(), False)
    assert material_id.is_valid(), path
    with sqlite3.connect((PROJECT / 'Cache/assetdb.sqlite').as_uri() + '?mode=ro', uri=True) as database:
        row = database.execute('SELECT s.SourceGuid,p.SubID FROM Products p JOIN Jobs j ON j.JobID=p.JobPK '
                               'JOIN Sources s ON s.SourceID=j.SourcePK WHERE p.ProductName=?', ('pc/' + path,)).fetchone()
    assert row is not None, path
    material = {'assetId': {'guid': '{' + str(uuid.UUID(bytes=row[0])).upper() + '}', 'subId': row[1]}, 'assetHint': path}
    assert silpom.DecalPaintRequestBus(bus.Event, 'BindReceiverUv', paint_canvas, tower,
                                      face['face'], azmath.Vector4(*map(float, rectangle)), 64)
    binding = json.loads(silpom.DecalPaintRequestBus(bus.Event, 'GetDocumentJson', paint_canvas))['ClassData']['ReceiverUv']
    def point(index, du, dv):
        return {'Position': [0, 0, 0], 'Normal': [0, 0, 1], 'Sequence': index, 'Start': True,
                'Surface': {'Receiver': binding['Receiver'], 'Face': face['face'],
                            'Uv': [uv[0] + du * ru, uv[1] + dv * rv], 'Front': front}}
    style = {'Material': material, 'Diameter': .055, 'Depth': .25, 'Rotation': 13.,
             'Opacity': .8, 'NormalOpacity': .2, 'HeightMask': 7}
    fixed = [dict(style, Point=point(i + 1, du, dv), Color=[.7, .12, .04], StrokeId=10)
             for i, (du, dv) in enumerate(((-.4, -.3), (.25, .2)))]
    live = dict(style, Id=20, Name='Live gradient probe', ExplicitDabs=True, PressureSize=False,
                Color=[.03, .15, .85], Points=[point(3, .4, -.3), point(4, -.25, .35)])
    document = {'Layers': [{'Id': 1, 'Name': 'Gradient probe', 'Order': [10, 20],
                            'Composite': fixed, 'Strokes': [live]}],
                'NextId': 100, 'FrozenThrough': 2, 'ReceiverUv': binding}
    paint_document(document)
    # Publishing the serialized document can replace the initial runtime
    # binding on its first controller tick, resetting its transient mode to 0.
    # Match receiver_uv_benchmark: settle real projections before selecting 2.
    general.idle_wait_frames(90)
    settled = paint_state()
    assert settled['fixed_dabs'] == 2 and settled['live_dabs'] == 2, settled
    assert settled['render_stats'].get('unique_projections') == 4, settled
    assert settled['render_stats'].get('batches', 0) > 0, settled
    probe.update(face=face, selected_pixel=[x, y, sample], front=front, uv=uv, rectangle=rectangle,
                 document=json.loads(silpom.DecalPaintRequestBus(bus.Event, 'GetDocumentJson', paint_canvas))['ClassData'],
                 settled_native_status=settled)
    assert silpom.SilPomMeshRequestBus(bus.Event, 'SetReceiverUvPaintMode', tower, 2)
    probe['receiver_after_mode_selection'] = paint_state()['receiver']
    assert probe['receiver_after_mode_selection']['mode'] == 2, probe
    deadline = time.monotonic() + 180
    while True:
        general.idle_wait_frames(5)
        state = paint_state()
        probe['status'] = state
        flush()
        if state['receiver'].get('mode') != 2:
            # Reapply only after a late binding publication reset the runtime
            # mode. Never touch a mode-2 atlas that is already baking or ready.
            probe.setdefault('late_mode_reselections', []).append(state['receiver'])
            assert silpom.SilPomMeshRequestBus(bus.Event, 'SetReceiverUvPaintMode', tower, 2)
            assert time.monotonic() < deadline, state
            continue
        if state['receiver'].get('ready') and state['receiver'].get('mode') == 2 and state['fixed_dabs'] == 2 and state['live_dabs'] == 2:
            break
        assert time.monotonic() < deadline, state
    probe.update(material_catalog_id=str(material_id), prefix_only_stem='2x-painted-lower-facing-prefix-only',
                 consumers_observed={'receiver_uv_bound': True, 'receiver_atlas_ready': True,
                                     'forward_composite_live_dabs': 2,
                                     'visible_atlas_and_live_contribution': 'requires offline sample comparisons'})
    general.idle_wait_frames(60)
    settled_atlas = paint_state()
    assert settled_atlas['receiver'].get('ready') and settled_atlas['receiver'].get('mode') == 2, settled_atlas
    assert settled_atlas['fixed_dabs'] == 2 and settled_atlas['live_dabs'] == 2, settled_atlas
    probe['status'] = settled_atlas


def cleanup_paint_probe():
    global paint_canvas
    if paint_canvas is None:
        return
    # Release the material binding even if paint submission/capture failed.
    silpom.SilPomMeshRequestBus(bus.Event, 'ReleaseReceiverUvPaint', tower)
    editor.ToolsApplicationRequestBus(bus.Broadcast, 'DeleteEntityById', paint_canvas)
    general.idle_wait_frames(5)
    query = entity.SearchFilter(); query.names = [PAINT_NAME]
    assert not entity.SearchBus(bus.Broadcast, 'SearchEntities', query), 'Paint probe entity survived cleanup'
    state = json.loads(silpom.SilPomMeshRequestBus(bus.Event, 'GetReceiverUvPaintStatus', tower))
    assert state['state'] == 'unbound', state
    paint_canvas = None
    result['paint_probe']['cleanup'] = {'entity_deleted': True, 'receiver_unbound': True}


def assert_controls(properties, configuration):
    expected = {
        'Lighting estimator': 5,
        'Height-derived normals (off = cheaper)': True,
        'Level 2 predictor (experimental)': configuration['UseLevelTwoPredictor'],
        'Gather validation (may be cheaper)': configuration['UseValidationGather'],
        'R16 height textures (less GPU memory)': configuration['UseR16HeightTextures'],
        'Level 4B shared material / visibility (experimental)': configuration['UseLevelFourShared'],
        'Level 5A learned relief LOD (experimental)': configuration['UseLevelFiveProxy'],
        'Level 5B bounded proxy depth (experimental)': configuration['UseLevelFiveDepth'],
    }
    actual = {}
    for label, value in expected.items():
        path = next(p for p in properties if p.endswith('|' + label))
        actual[label] = read(path)
        assert actual[label] == value, (label, actual[label], value)
    return actual


def main():
    global tower, component, normals_path, original_normals
    source = LEVEL.read_bytes()
    fixture = json.loads(LEVEL.with_name('scene.json').read_text())
    configuration = fixture['tower_configuration']
    assert configuration['MaxCells'] == 65525 or configuration.get('ReliefMode') == 8, 'Requires tower full-screen resolve mode'
    result['level_sha256'] = hashlib.sha256(source).hexdigest()
    result['scene_sha256'] = result['level_sha256']
    assert result['scene_sha256'] == fixture['scene_sha256']
    general.idle_enable(True)
    for name, value in (('ed_keepEditorActive', 1), ('ed_backgroundUpdatePeriod', -1),
                        ('r_displayInfo', 0), ('vsync_interval', 0), ('sys_MaxFPS', -1),
                        ('r_silpomPaintBake', 0), ('r_silpomMsaaInspect', 0),
                        ('r_silpomResolveRegionTrace', 0), ('r_silpomResolveMultiRect', 1)):
        command(name, value)
    command('r_silpomFaceCache', int(result['face_cache_enabled']))
    command('r_silpomFaceCacheTrace', 1)
    assert general.open_level(str(LEVEL))
    general.idle_wait_frames(120)
    position, rotation = general.get_current_view_position(), general.get_current_view_rotation()
    size = general.get_viewport_size()
    initial_view.update(position=(position.x, position.y, position.z),
                        rotation=(rotation.x, rotation.y, rotation.z),
                        size=(int(size.x), int(size.y)), policy=general.get_viewport_expansion_policy())
    query = entity.SearchFilter()
    query.names = ['Irish Fieldstone Tower (Open Shaft)']
    towers = entity.SearchBus(bus.Broadcast, 'SearchEntities', query)
    assert len(towers) == 1
    tower = towers[0]
    ready()
    result['initial_aa_state'] = aa_state()
    kind = editor.EditorComponentAPIBus(bus.Broadcast, 'FindComponentTypeIdsByEntityType',
                                      ['SilPOM Mesh'], entity.EntityType().Game)[0]
    found = editor.EditorComponentAPIBus(bus.Broadcast, 'GetComponentOfType', tower, kind)
    assert found.IsSuccess()
    component = found.GetValue()
    properties = editor.EditorComponentAPIBus(bus.Broadcast, 'BuildComponentPropertyList', component)
    normals_path = next(p for p in properties if p.endswith('|Height-derived normals (off = cheaper)'))
    original_normals = read(normals_path)
    result['saved_height_derived_normals'] = original_normals
    result['requested_overrides'] = {'UseHeightDerivedNormals': True}
    effective = dict(configuration)
    effective['UseHeightDerivedNormals'] = True
    result['tower_configuration_sha256'] = fixture['tower_configuration_sha256']
    result['effective_tower_configuration_sha256'] = hashlib.sha256(
        json.dumps(effective, sort_keys=True).encode()).hexdigest()
    if not original_normals:
        write(normals_path, True)
    ready()
    result['controls'] = assert_controls(properties, configuration)
    result['tower_status'] = str(silpom.SilPomMeshRequestBus(bus.Event, 'GetStatus', tower))

    # Match the benchmark's renderer policy without changing the saved component.
    command('r_silpomAntiAliasing', 4)
    ready([2, 0, 0])
    result['aa_state'] = aa_state()
    assert str(general.get_cvar('r_silpomResolveMultiRect')).lower() in ('true', '1', '1.0')
    result['scheduler'] = 'disjoint_rectangles'
    result['receiver_uv'] = json.loads(silpom.SilPomMeshRequestBus(bus.Event, 'GetReceiverUvPaintStatus', tower))
    assert result['receiver_uv']['state'] == 'unbound', result['receiver_uv']
    command('r_silpomMsaaInspect', 1)
    command('r_silpomMsaaRawCoverage', 1)
    general.close_pane('Console')
    general.set_viewport_expansion_policy('FixedSize')
    general.set_viewport_size(*VIEWPORT)
    cases = [(name, fixture['views'][name], 'fixed-benchmark') for name in VIEWS]
    if os.environ.get('SILPOM_GRADIENT_DIAGNOSTIC', '1') != '0':
        pose = dict(fixture['views']['exterior'])
        pose['position'] = [pose['position'][0] * 2, pose['position'][1] * 2, pose['position'][2]]
        cases.append(('wide-representations', pose, 'representation-probe'))
    if result['paint_probe_enabled']:
        cases.append(('painted-lower-facing', fixture['views']['lower-facing'], 'paint-probe'))
    result['view_poses'] = {name: pose for name, pose, _ in cases}
    result['source_sha256'] = {}
    gem = silpom_paths.GEM
    for relative in (
        'Assets/Materials/Types/SilPOMMesh/SilPOMMesh.azsli',
        'Assets/Materials/Types/SilPOMMesh/SilPOMMesh_DirectionalLighting.azsli',
        'Assets/Shaders/SilPOM/GradientContext.azsli',
        'Assets/Shaders/SilPOM/RepairGradientContext.azsli',
        'Assets/Shaders/SilPOM/GradientConsumers.azsli',
        'Assets/Shaders/SilPOM/StandardPbrNormal.azsli',
        'Assets/Shaders/SilPOM/MaterialNormalDecode.azsli',
        'Assets/Shaders/SilPOM/LearnedNormalFootprint.azsli',
        'Assets/Shaders/SilPOM/NormalQueryPolicy.azsli',
        'Assets/Shaders/SilPOM/FaceCoefficients.azsli',
        'Assets/Shaders/SilPOM/ImplicitProxyCoverage.azsli',
        'Assets/Shaders/SilPOM/HitReuse/SilPOMFaceCachePrepare.azsl',
        'Assets/Shaders/SilPOM/HitReuse/SilPOMFaceCacheRead.azsli',
        'Assets/Passes/SilPOMFaceCache.pass',
        'Assets/Shaders/SilPOM/LearnedMaterialSurface.azsli',
        'Assets/Shaders/SilPOM/ProxyAppearance.azsli',
        'Assets/Shaders/SilPOM/ForwardLighting.azsli',
        'Assets/Shaders/SilPOM/ReceiverUv/PaintSurface.azsli',
        'Assets/Shaders/SilPOM/ReceiverUv/BakePipeline.azsli',
        'Assets/Shaders/SilPOM/ReceiverUv/UsagePipeline.azsli',
        'Assets/Materials/DecalPaint/hard_round.material',
        'Assets/Materials/DecalPaint/hard_round.png',
        'Assets/Materials/Pipelines/MainPipeline/SilPOMHitForward.azsli',
        'Assets/Materials/Pipelines/MainPipeline/SilPOMHitForwardAngularProxyAmbient.azsli',
        'Assets/Materials/Pipelines/MainPipeline/SilPOMHitDepthProxy.azsli',
        'Assets/Shaders/SilPOM/HitReuse/SilPOMClassifyWallHoles.azsl',
        'Assets/Shaders/SilPOM/HitReuse/SilPOMFillWallHoles.azsl',
        'Assets/Shaders/SilPOM/HitReuse/SilPOMInspectSamples.azsl',
        'Assets/Passes/SilPOMHitDepth.pass',
        'Assets/Passes/SilPOMHitDepthMsaa.pass',
        'Assets/Passes/SilPOMHitDepthWallBackup.pass',
        'Assets/Passes/SilPOMHitForward.pass',
        'Assets/Passes/SilPOMClassifyWallHolesMsaa.pass',
        'Assets/Passes/SilPOMFillWallHoles.pass',
        'Assets/Passes/SilPOMInspectSamples.pass',
        'Assets/Passes/SilPOMInspectUvUsage.pass',
        'Source/ResolvePass.cpp',
    ):
        result['source_sha256'][relative] = hashlib.sha256((gem / relative).read_bytes()).hexdigest()
    import o3de_patches
    result['o3de_patches'] = o3de_patches.evidence()
    for name, pose, fixture_kind in cases:
        overrides = {}
        if fixture_kind == 'paint-probe':
            restore_properties()
            ready([2, 0, 0])
        if fixture_kind == 'representation-probe':
            overrides = {
                'Level 5B bounded proxy depth (experimental)': False,
                'Level 5A error budget (higher = more proxies)': 4.0,
                'Relief LOD pixel budget (higher = cheaper)': 64.0,
            }
            for label, value in overrides.items():
                temporary_property(properties, label, value)
            ready([2, 0, 0])
        result['stage'] = name + '/settling'
        flush()
        general.set_current_view_position(*map(float, pose['position']))
        general.set_current_view_rotation(*map(float, pose['rotation']))
        general.idle_wait_frames(120)
        actual = general.get_viewport_size()
        assert (int(actual.x), int(actual.y)) == VIEWPORT
        if fixture_kind == 'paint-probe':
            prepare_paint_probe()
        for label, enabled in (('full', 0), ('region', 1), ('control', 0)):
            stem = f'2x-{name}-{label}'
            result['stage'] = stem
            flush()
            command('r_silpomResolveRegions', enabled)
            cache_enabled = result['face_cache_enabled']
            trace_start = (PROJECT / 'user/log/Editor.log').stat().st_size
            if result['face_cache_validation_sequence'] == 'off/on/off':
                cache_enabled = label == 'region'
                command('r_silpomFaceCache', int(cache_enabled))
            general.idle_wait_frames(65 if cache_enabled and result['face_cache_validation_sequence'] == 'off/on/off' else 30)
            renderer_clean()
            assert aa_state() == [2, 0, 0]
            assert read(normals_path) is True
            if label == 'region':
                command('r_silpomResolveRegionTrace', 1)
                general.idle_wait_frames(2)
                command('r_silpomResolveRegionTrace', 0)
            snapshot(stem)
            if fixture_kind == 'paint-probe':
                capture(OUTPUT / f'{stem}-usage.dds', 'Records',
                        ['Root', 'MainPipeline_0', 'OpaquePass', 'SilPOMInspectUvUsage'])
                status = paint_state()
                assert status['receiver'].get('ready') and status['receiver'].get('mode') == 2, status
                assert status['fixed_dabs'] == 2 and status['live_dabs'] == 2, status
                result['paint_probe'].setdefault('run_status', {})[label] = status
            cache_trace = {}
            if cache_enabled and result['face_cache_validation_sequence'] == 'off/on/off':
                with (PROJECT / 'user/log/Editor.log').open('rb') as stream:
                    stream.seek(trace_start)
                    text = stream.read().decode('utf-8', errors='replace')
                prepared = {int(v) for v in re.findall(r'FaceCache pass=.*?tick=(\d+).*?output=ready table=ready', text)}
                depth = {int(v) for v in re.findall(r'FaceCacheBind pass=\S*\.SilPOMHitDepth view=.*?tick=(\d+) batches=[1-9]\d* resources=ready', text)}
                forward = {int(v) for v in re.findall(r'FaceCacheBind pass=\S*\.SilPOMHitForward view=.*?tick=(\d+) batches=[1-9]\d* resources=ready', text)}
                active = sorted(prepared & depth & forward)
                assert active, 'No same-frame cache preparation and depth/forward binding for ' + stem
                cache_trace = {'same_frame_preparation_depth_forward_ticks': active}
            result['runs'].append({'stem': stem, 'view': name, 'mode': label,
                                   'samples': SAMPLES, 'width': VIEWPORT[0], 'height': VIEWPORT[1],
                                   'face_cache_enabled': cache_enabled, 'face_cache_trace': cache_trace,
                                   'fixture': fixture_kind, 'temporary_overrides': overrides,
                                   'legacy_view': name if fixture_kind == 'fixed-benchmark' else None})
            flush()
        if fixture_kind == 'paint-probe':
            # Retain the same fixed prefix and remove only the forward live tail.
            prefix_only = copy.deepcopy(result['paint_probe']['document'])
            prefix_only['Layers'][0]['Strokes'] = []
            prefix_only['Layers'][0]['Order'] = [10]
            paint_document(prefix_only)
            command('r_silpomResolveRegions', 0)
            general.idle_wait_frames(60)
            status = paint_state()
            assert status['fixed_dabs'] == 2 and status['live_dabs'] == 0, status
            assert status['receiver'].get('ready') and status['receiver'].get('mode') == 2, status
            assert status['receiver']['bake_ms'] == result['paint_probe']['status']['receiver']['bake_ms'], 'Live-tail edit rebuilt the fixed atlas'
            snapshot(result['paint_probe']['prefix_only_stem'], ('diffuse', 'normal'))
            result['paint_probe']['prefix_only_status'] = status
            result['paint_probe']['passed'] = True
            cleanup_paint_probe()
    renderer_clean()
    log = (PROJECT / 'user/log/Editor.log').read_text(errors='replace')
    counts = [int(value) for value in re.findall(r'ResolveRegions .*?replaced=(\d+).*?indices=3', log)]
    result['traced_replacement_counts'] = counts
    assert any(value > 0 for value in counts), 'No active three-index region draw'
    rectangles = [(int(batches), int(draws)) for batches, draws in re.findall(
        r'ResolveRegions .*?replaced=(\d+) rectangles=(\d+) multi=1', log)]
    result['traced_rectangle_counts'] = rectangles
    assert any(draws > batches for batches, draws in rectangles), 'No batch used multiple rectangles'
    restore_properties()
    ready([2, 0, 0])
    assert result['controls'] == assert_controls(properties, configuration)
    assert LEVEL.read_bytes() == source, 'Saved level changed'
    result.update(passed=True, stage='complete', saved_level_unchanged=True)


try:
    flush()
    main()
except Exception:
    result['error'] = traceback.format_exc()
finally:
    # An optional probe failure must not prevent the original settings cleanup.
    try:
        cleanup_paint_probe()
    except Exception:
        result['paint_cleanup_error'] = traceback.format_exc()
        result['passed'] = False
    try:
        command('r_silpomMsaaInspect', 0)
        general.idle_wait_frames(12)
        if component is not None:
            restore_properties()
        if component is not None and normals_path is not None and original_normals is not None:
            write(normals_path, original_normals)
            ready()
            result['normals_restored'] = read(normals_path) == original_normals
            assert result['normals_restored']
        if 'r_silpomAntiAliasing' in cvars:
            general.run_console(f"r_silpomAntiAliasing {cvars['r_silpomAntiAliasing']}")
            general.idle_wait_frames(120)
        for name, value in cvars.items():
            if name not in ('r_silpomAntiAliasing', 'ed_keepEditorActive', 'ed_backgroundUpdatePeriod'):
                general.run_console(f'{name} {value}')
        if initial_view:
            general.set_current_view_position(*initial_view['position'])
            general.set_current_view_rotation(*initial_view['rotation'])
            general.set_viewport_expansion_policy(initial_view['policy'])
            general.idle_wait_frames(30)
            if initial_view['policy'] == 'FixedSize':
                general.set_viewport_size(*initial_view['size'])
                general.idle_wait_frames(30)
        for name in ('ed_keepEditorActive', 'ed_backgroundUpdatePeriod'):
            if name in cvars:
                general.run_console(f'{name} {cvars[name]}')
        result['restored'] = True
        result['temporary_properties_restored'] = not property_restores
    except Exception:
        result['restore_error'] = traceback.format_exc()
        result['passed'] = False
    flush()
    from PySide6 import QtCore
    QtCore.QTimer.singleShot(0, general.exit_no_prompt)

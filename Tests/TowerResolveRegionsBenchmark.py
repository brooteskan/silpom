"""Native GPU timestamps for original versus scissored tower batch resolves.

Run in a dedicated Editor --runpython with TG_SILPOM_RESOLVE_BENCH_OUTPUT set
to a fresh directory. Optional TG_SILPOM_RESOLVE_BENCH_VIEWS (comma-separated
fixture names), TG_SILPOM_RESOLVE_BENCH_REPEATS (>=3, default3), and
TG_SILPOM_RESOLVE_BENCH_FRAMES (>=120, default120). No inspection attachments,
screenshots, RenderDoc, scene saves, or asset reads occur during measurement.
"""
import hashlib
import json
import os
from pathlib import Path
import time
import traceback

import azlmbr.bus as bus
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.paths as paths
import azlmbr.silpom as silpom

PROJECT = Path(paths.projectroot)
GEM = PROJECT / 'Gems/silpom'
LEVEL = PROJECT / 'Levels/SilPOMTowerBenchmark/SilPOMTowerBenchmark.prefab'
OUTPUT = Path(os.environ['TG_SILPOM_RESOLVE_BENCH_OUTPUT'])
OUTPUT.mkdir(parents=True, exist_ok=True)
assert not (OUTPUT / 'results.json').exists(), 'Use a fresh benchmark directory'
result = {'passed': False, 'stage': 'startup', 'runs': [], 'orders': [],
          'benchmark_performed': False, 'timing_only': True, 'samples': 2,
          'viewport': [1280, 720], 'inspector_during_timing': False,
          'gradient_contract': 'owner-local-first-order-per-raster-pixel',
          'normal_query_policy': 'prepared-normal-stencil-with-probe-validity',
          'face_cache_policy': 'generation-coefficients-frame-local-implicit-near-v1',
          'timing_scope': 'Native GPU pass timestamp queries; graphics span includes queue gaps; CPU values are RHI frame periods including waits'}
cvars, originals, initial_view = {}, [], {}
tower = None
component = None


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


def aa_state():
    state = silpom.SilPomMeshRequestBus(bus.Event, 'GetAntiAliasingState', tower)
    return [int(state.x), int(state.y), int(state.z)]


def ready(expected_aa=None):
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        general.idle_wait_frames(1)
        if silpom.SilPomMeshRequestBus(bus.Event, 'IsReady', tower):
            if expected_aa is None or aa_state() == expected_aa:
                return
    raise AssertionError(str(silpom.SilPomMeshRequestBus(bus.Event, 'GetStatus', tower)))


def renderer_clean():
    log = (PROJECT / 'user/log/Editor.log').read_text(errors='replace')
    assert not any(error in log for error in (
        'Failed to bind shader resources for draw item',
        '[Error] (DeviceShaderResourceGroupData)', 'Trace::Assert',
        'DXGI_ERROR_DEVICE_REMOVED', 'DXGI_ERROR_DEVICE_HUNG')), 'Renderer error; inspect Editor.log'


def false_cvar(name):
    return str(general.get_cvar(name)).lower() in ('false', '0', '0.0')


def assert_capture_state(mode):
    assert aa_state() == [2, 0, 0], aa_state()
    size = general.get_viewport_size()
    assert [int(size.x), int(size.y)] == [1280, 720]
    for name in ('r_silpomMsaaInspect', 'r_silpomResolveRegionTrace', 'r_silpomPaintBake'):
        assert false_cvar(name), (name, str(general.get_cvar(name)))
    assert not false_cvar('r_silpomResolveMultiRect'), 'The disjoint rectangle scheduler must be enabled'
    assert false_cvar('r_silpomResolveRegions') == (mode == 'full')
    paint = json.loads(silpom.SilPomMeshRequestBus(bus.Event, 'GetReceiverUvPaintStatus', tower))
    assert paint['state'] == 'unbound', paint


def capture(view_name, repeat, mode, frames):
    result['stage'] = f'{view_name}/repeat-{repeat}/{mode}'
    flush()
    command('r_silpomResolveRegions', 0 if mode == 'full' else 1)
    # These waits and trace logging are outside the timestamp interval. Query
    # collection also discards16frames internally before accepting fresh data.
    if mode == 'region' and repeat == 1:
        command('r_silpomResolveRegionTrace', 1)
        general.idle_wait_frames(2)
        command('r_silpomResolveRegionTrace', 0)
    general.idle_wait_frames(30)
    renderer_clean()
    assert_capture_state(mode)
    metadata = {'view': view_name, 'repeat': repeat, 'mode': mode,
                'resolve_regions': mode == 'region', 'aa_state': aa_state(),
                'effective_tower_configuration_sha256': result['effective_tower_configuration_sha256'],
                'viewport': [1280, 720], 'inspection': False, 'timing_only': True,
                'scheduler': 'disjoint_rectangles'}
    started = time.monotonic()
    assert silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, 'BeginCapture', frames)
    deadline = time.monotonic() + 240
    while not silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, 'IsCaptureDone'):
        assert time.monotonic() < deadline, 'Native GPU timestamp capture timed out'
        general.idle_wait_frames(1)
    data = json.loads(silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, 'EndCapture'))
    data['editor_capture_wall_seconds'] = time.monotonic() - started
    data['capture_metadata'] = metadata
    assert data['complete'] and data['frames'] == frames
    assert data['depth_attachment'] == {'width': 1280, 'height': 720, 'msaa': 2}
    intrusive = {index for index, item in enumerate(data['passes'])
                 if any(token in item['path'].lower() for token in ('silpominspect', 'silpomuvusage', 'readback'))}
    assert not any(sample[1] in intrusive and sample[4] > 0 for sample in data['samples']), \
        'An inspection/readback pass contributed GPU time'
    filename = f'gpu-{view_name}-r{repeat}-{mode}.json'
    (OUTPUT / filename).write_text(json.dumps(data, indent=2), encoding='utf-8')
    result['runs'].append({'view': view_name, 'repeat': repeat, 'mode': mode,
                           'file': filename, 'capture_metadata': metadata,
                           'sha256': hashlib.sha256((OUTPUT / filename).read_bytes()).hexdigest()})
    result['hardware'] = data['hardware']
    result['benchmark_performed'] = True
    assert_capture_state(mode)
    renderer_clean()
    flush()


def main():
    global tower, component
    source = LEVEL.read_bytes()
    vue_path = PROJECT / 'Levels/VueLiveLink/VueLiveLink.prefab'
    vue_source = vue_path.read_bytes()
    fixture = json.loads(LEVEL.with_name('scene.json').read_text())
    assert hashlib.sha256(source).hexdigest() == fixture['scene_sha256']
    result['scene_sha256'] = fixture['scene_sha256']
    result['tower_configuration_sha256'] = fixture['tower_configuration_sha256']
    result['face_cache_enabled'] = general.get_cvar('r_silpomFaceCache')
    result['source_sha256'] = {str(path.relative_to(GEM)).replace('\\', '/'):
        hashlib.sha256(path.read_bytes()).hexdigest() for path in (
            GEM / 'Source/ResolvePass.cpp', GEM / 'Source/Mesh.cpp',
            GEM / 'Include/SilPOM/ResolveRegion.h',
            GEM / 'Include/SilPOM/ResolveRegions.h',
            GEM / 'Assets/Materials/Types/SilPOMMesh/SilPOMMesh.azsli',
            GEM / 'Assets/Shaders/SilPOM/GradientContext.azsli',
            GEM / 'Assets/Shaders/SilPOM/RepairGradientContext.azsli',
            GEM / 'Assets/Shaders/SilPOM/GradientConsumers.azsli',
            GEM / 'Assets/Shaders/SilPOM/StandardPbrNormal.azsli',
            GEM / 'Assets/Shaders/SilPOM/MaterialNormalDecode.azsli',
            GEM / 'Assets/Shaders/SilPOM/LearnedNormalFootprint.azsli',
            GEM / 'Assets/Shaders/SilPOM/NormalQueryPolicy.azsli',
            GEM / 'Assets/Shaders/SilPOM/FaceCoefficients.azsli',
            GEM / 'Assets/Shaders/SilPOM/ImplicitProxyCoverage.azsli',
            GEM / 'Assets/Shaders/SilPOM/HitReuse/SilPOMFaceCachePrepare.azsl',
            GEM / 'Assets/Shaders/SilPOM/HitReuse/SilPOMFaceCacheRead.azsli',
            GEM / 'Assets/Shaders/SilPOM/LearnedMaterialSurface.azsli',
            GEM / 'Assets/Shaders/SilPOM/BlendedFace.azsli',
            GEM / 'Assets/Shaders/SilPOM/ProxyAppearance.azsli',
            GEM / 'Assets/Materials/Pipelines/MainPipeline/SilPOMHitForward.azsli',
            GEM / 'Assets/Materials/Pipelines/MainPipeline/SilPOMHitDepth.azsli',
            GEM / 'Assets/Materials/Pipelines/MainPipeline/SilPOMHitDepthProxy.azsli',
            GEM / 'Assets/Shaders/SilPOM/HitReuse/SilPOMClassifyWallHoles.azsl',
            GEM / 'Integration/O3DE/manifest.json',
            GEM / 'Assets/Passes/SilPOMHitForward.pass')}
    repeats = int(os.environ.get('TG_SILPOM_RESOLVE_BENCH_REPEATS', '3'))
    frames = int(os.environ.get('TG_SILPOM_RESOLVE_BENCH_FRAMES', '120'))
    assert repeats >= 3 and frames >= 120
    names = os.environ.get('TG_SILPOM_RESOLVE_BENCH_VIEWS',
                           'exterior,lower-facing,lower-glancing,upper-glancing').split(',')
    assert names and all(name in fixture['views'] for name in names)
    result.update(repeats=repeats, frames_per_capture=frames,
                  views={name: fixture['views'][name] for name in names},
                  warmup={'camera_frames': 90, 'mode_frames': 30, 'native_query_frames': 16})
    general.idle_enable(True)
    for name, value in (('ed_keepEditorActive', 1), ('ed_backgroundUpdatePeriod', -1),
                        ('r_displayInfo', 0), ('vsync_interval', 0), ('sys_MaxFPS', -1),
                        ('r_silpomPaintBake', 0), ('r_silpomMsaaInspect', 0),
                        ('r_silpomResolveRegionTrace', 0), ('r_silpomResolveMultiRect', 1)):
        command(name, value)
    assert general.open_level(str(LEVEL))
    general.idle_wait_frames(90)
    position, rotation, size = (general.get_current_view_position(),
                                general.get_current_view_rotation(), general.get_viewport_size())
    initial_view.update(position=(position.x, position.y, position.z),
                        rotation=(rotation.x, rotation.y, rotation.z),
                        policy=general.get_viewport_expansion_policy(), size=(int(size.x), int(size.y)))
    query = entity.SearchFilter()
    query.names = ['Irish Fieldstone Tower (Open Shaft)']
    towers = entity.SearchBus(bus.Broadcast, 'SearchEntities', query)
    assert len(towers) == 1
    tower = towers[0]
    ready()
    result['initial_aa_state'] = aa_state()
    result['initial_renderer_cvars'] = {name: str(general.get_cvar(name)) for name in (
        'r_silpomResolveRegions', 'r_multiSampleCount', 'r_antiAliasing', 'r_silpomAntiAliasing')}
    command('r_silpomAntiAliasing', 4)
    ready([2, 0, 0])
    kind = editor.EditorComponentAPIBus(bus.Broadcast, 'FindComponentTypeIdsByEntityType',
                                      ['SilPOM Mesh'], entity.EntityType().Game)[0]
    component = editor.EditorComponentAPIBus(bus.Broadcast, 'GetComponentOfType', tower, kind).GetValue()
    props = editor.EditorComponentAPIBus(bus.Broadcast, 'BuildComponentPropertyList', component)
    normals = next(path for path in props if path.endswith('|Height-derived normals (off = cheaper)'))
    original_normals = read(normals)
    result['saved_height_derived_normals'] = original_normals
    result['requested_overrides'] = {'UseHeightDerivedNormals': True}
    if not original_normals:
        originals.append((normals, original_normals))
        write(normals, True)
    ready([2, 0, 0])
    cfg = dict(fixture['tower_configuration'])
    cfg['UseHeightDerivedNormals'] = True
    assert cfg['MaxCells'] == 65525
    result['effective_tower_configuration_sha256'] = hashlib.sha256(json.dumps(cfg, sort_keys=True).encode()).hexdigest()
    expected = {'Lighting estimator': 5, 'Height-derived normals (off = cheaper)': True,
                'Level 2 predictor (experimental)': cfg['UseLevelTwoPredictor'],
                'Gather validation (may be cheaper)': cfg['UseValidationGather'],
                'R16 height textures (less GPU memory)': cfg['UseR16HeightTextures']}
    result['controls'] = {}
    for label, value in expected.items():
        path = next(path for path in props if path.endswith('|' + label))
        actual = read(path)
        assert actual == value, (label, actual, value)
        result['controls'][label] = actual
    result['controls']['Anti-aliasing'] = 4
    result['tower_status'] = str(silpom.SilPomMeshRequestBus(bus.Event, 'GetStatus', tower))
    general.set_viewport_expansion_policy('FixedSize')
    general.set_viewport_size(1280, 720)
    general.close_pane('Console')
    assert silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, 'CreateReference', tower,
        'assets/silpomplanarab/irishfieldstonetowerwelded/wall/wall.azmaterial', 1, 1.0, 1.0)
    deadline = time.monotonic() + 120
    while not silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, 'IsReady'):
        assert time.monotonic() < deadline
        general.idle_wait_frames(1)
    for view_index, name in enumerate(names):
        pose = fixture['views'][name]
        general.set_current_view_position(*map(float, pose['position']))
        general.set_current_view_rotation(*map(float, pose['rotation']))
        general.idle_wait_frames(90)
        for repeat in range(1, repeats + 1):
            modes = ['full', 'region'] if (view_index + repeat) % 2 else ['region', 'full']
            result['orders'].append({'view': name, 'repeat': repeat, 'modes': modes})
            for mode in modes:
                capture(name, repeat, mode, frames)
    renderer_clean()
    assert LEVEL.read_bytes() == source and vue_path.read_bytes() == vue_source
    result.update(passed=True, stage='complete', saved_level_unchanged=True, vue_live_link_unchanged=True)


try:
    flush()
    main()
except Exception:
    result['error'] = traceback.format_exc()
finally:
    try:
        silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, 'Release')
        for path, value in reversed(originals):
            write(path, value)
        if originals:
            ready()
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
            if initial_view['policy'] == 'FixedSize':
                general.set_viewport_size(*initial_view['size'])
            general.idle_wait_frames(30)
        for name in ('ed_keepEditorActive', 'ed_backgroundUpdatePeriod'):
            if name in cvars:
                general.run_console(f'{name} {cvars[name]}')
        result['restored'] = True
    except Exception:
        result['restore_error'] = traceback.format_exc()
        result['passed'] = False
    flush()
    from PySide6 import QtCore
    QtCore.QTimer.singleShot(0, general.exit_no_prompt)

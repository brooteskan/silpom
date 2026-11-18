"""Focused 2x MSAA tower full/disjoint/full repair correctness readbacks.

Only repair scheduling changes. No GPU timestamps or performance measurements.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import time
import traceback

import azlmbr.atom as atom
import azlmbr.bus as bus
import azlmbr.entity as entity
import azlmbr.editor as editor
import azlmbr.legacy.general as general
import azlmbr.paths as paths
import azlmbr.silpom as silpom

PROJECT = Path(paths.projectroot)
LEVEL = PROJECT / 'Levels/SilPOMTowerBenchmark/SilPOMTowerBenchmark.prefab'
OUTPUT = Path(os.environ['SILPOM_REPAIR_OUTPUT'])
assert not (OUTPUT / 'results.json').exists(), 'Use a fresh correctness directory'
OUTPUT.mkdir(parents=True, exist_ok=True)
result = {'passed': False, 'stage': 'startup', 'runs': [], 'benchmark_performed': False, 'repair_traces': {},
          'comparison': 'full_screen_vs_disjoint_repair', 'samples': 2}
cvars = {}
initial_view = {}
tower = None
component = None
normals_path = None
original_normals = None


def flush():
    (OUTPUT / 'results.json').write_text(json.dumps(result, indent=2), encoding='utf-8')


def command(name, value):
    if name not in cvars:
        cvars[name] = general.get_cvar(name)
    general.run_console(f'{name} {value}')


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


def ready(samples=None):
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        general.idle_wait_frames(1)
        if not silpom.SilPomMeshRequestBus(bus.Event, 'IsReady', tower):
            continue
        state = silpom.SilPomMeshRequestBus(bus.Event, 'GetAntiAliasingState', tower)
        if samples is None or int(state.x) == samples:
            general.idle_wait_frames(30)
            return
    raise AssertionError(str(silpom.SilPomMeshRequestBus(bus.Event, 'GetStatus', tower)))


def renderer_clean():
    log = (PROJECT / 'user/log/Editor.log').read_text(errors='replace')
    errors = ('Failed to bind shader resources for draw item',
              '[Error] (DeviceShaderResourceGroupData)', 'Trace::Assert')
    assert not any(error in log for error in errors), 'Renderer binding/assert error; inspect Editor.log'


def snapshot(stem):
    records = ['Root', 'MainPipeline_0', 'DepthPrePass', 'SilPOMInspectFinal']
    colors = ['Root', 'MainPipeline_0', 'OpaquePass', 'SilPOMInspectColors']
    for suffix, slot, hierarchy in (
        ('hit', 'Records', records), ('depth', 'SampleDepth', records),
        ('diffuse', 'DiffuseSamples', colors), ('specular', 'SpecularSamples', colors),
        ('normal', 'NormalSamples', colors),
    ):
        capture(OUTPUT / f'{stem}-{suffix}.dds', slot, hierarchy)


def main():
    global tower, component, normals_path, original_normals
    source = LEVEL.read_bytes()
    fixture = json.loads(LEVEL.with_name('scene.json').read_text())
    result['level_sha256'] = hashlib.sha256(source).hexdigest()
    general.idle_enable(True)
    for name, value in (('ed_keepEditorActive', 1), ('ed_backgroundUpdatePeriod', -1),
                        ('r_displayInfo', 0), ('vsync_interval', 0), ('sys_MaxFPS', -1),
                        ('r_silpomPaintBake', 0), ('r_silpomMsaaInspect', 0),
                        ('r_silpomRepairRegionTrace', 0), ('r_silpomResolveRegionTrace', 0),
                        ('r_silpomResolveRegions', 1), ('r_silpomResolveMultiRect', 1),
                        ('r_silpomFaceCache', 1), ('r_silpomFaceCacheTrace', 0)):
        command(name, value)
    assert general.open_level(str(LEVEL))
    general.idle_wait_frames(60)
    position = general.get_current_view_position()
    rotation = general.get_current_view_rotation()
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
    kind = editor.EditorComponentAPIBus(bus.Broadcast, 'FindComponentTypeIdsByEntityType',
                                      ['SilPOM Mesh'], entity.EntityType().Game)[0]
    component = editor.EditorComponentAPIBus(bus.Broadcast, 'GetComponentOfType', tower, kind).GetValue()
    properties = editor.EditorComponentAPIBus(bus.Broadcast, 'BuildComponentPropertyList', component)
    normals_path = next(p for p in properties if p.endswith('|Height-derived normals (off = cheaper)'))
    response = editor.EditorComponentAPIBus(bus.Broadcast, 'GetComponentProperty', component, normals_path)
    assert response.IsSuccess()
    original_normals = response.GetValue()
    if not original_normals:
        assert editor.EditorComponentAPIBus(bus.Broadcast, 'SetComponentProperty', component, normals_path, True).IsSuccess()
        ready()
    estimator_path = next(p for p in properties if p.endswith('|Lighting estimator'))
    estimator = editor.EditorComponentAPIBus(bus.Broadcast, 'GetComponentProperty', component, estimator_path).GetValue()
    assert estimator == 5, ('Expected AngularEdges4', estimator)
    result['controls'] = {'lighting_estimator': 'AngularEdges4', 'height_derived_normals': True,
                          'face_cache': True, 'resolve_regions': True, 'resolve_multi_rect': True}
    result['tower_status'] = str(silpom.SilPomMeshRequestBus(bus.Event, 'GetStatus', tower))
    assert fixture['tower_configuration']['MaxCells'] == 65525 or fixture['tower_configuration'].get('ReliefMode') == 8, 'Requires tower full-screen resolve mode'
    assert str(general.get_cvar('r_silpomResolveRegions')).lower() in ('true', 'false', '0', '1', '0.0', '1.0')
    command('r_silpomMsaaInspect', 1)
    command('r_silpomMsaaRawCoverage', 1)
    general.close_pane('Console')
    general.set_viewport_expansion_policy('FixedSize')
    samples_list = [2]
    names = ['exterior', 'lower-facing', 'lower-glancing', 'upper-glancing']
    result['view_poses'] = {name: fixture['views'][name] for name in names}
    views = [(name, fixture['views'][name], (1280, 720)) for name in names]
    for samples in samples_list:
        assert samples in (1, 2, 4)
        # Use the renderer's guarded sample-count transition. SMAA at 1x is
        # downstream of the captured sample attachments and cannot mask errors.
        command('r_silpomAntiAliasing', {1: 1, 2: 4, 4: 3}[samples])
        ready(samples)
        for name, pose, dimensions in views:
            general.set_viewport_size(*dimensions)
            general.set_current_view_position(*map(float, pose['position']))
            general.set_current_view_rotation(*map(float, pose['rotation']))
            general.idle_wait_frames(60)
            actual = general.get_viewport_size()
            assert (int(actual.x), int(actual.y)) == dimensions, (name, actual)
            for label, enabled in (('full', 0), ('region', 1), ('control', 0)):
                stem = f'{samples}x-{name}-{label}'
                result['stage'] = stem
                flush()
                command('r_silpomRepairRegions', enabled)
                general.idle_wait_frames(15)
                renderer_clean()
                if label == 'region':
                    log_path = PROJECT / 'user/log/Editor.log'
                    offset = log_path.stat().st_size
                    command('r_silpomRepairRegionTrace', 1)
                    general.idle_wait_frames(3)
                    command('r_silpomRepairRegionTrace', 0)
                    fresh = log_path.read_bytes()[offset:].decode(errors='replace')
                    traces = re.findall(r'RepairRegion tick=(\d+) mode=(\d+) paired=(\d+) producers=(\d+) rectangles=(\d+) scheduledPixels=(\d+)', fresh)
                    result['repair_traces'][name] = [dict(zip(('tick','mode','paired','producers','rectangles','pixels'), map(int, t))) for t in traces]
                    assert any(t['mode'] == 1 and t['paired'] == 1 and t['producers'] > 0 and t['rectangles'] > 0
                               for t in result['repair_traces'][name]), ('Regional repair inactive', name, result['repair_traces'][name])
                snapshot(stem)
                result['runs'].append({'stem': stem, 'view': name, 'mode': label, 'samples': samples,
                                       'width': dimensions[0], 'height': dimensions[1]})
                flush()
    renderer_clean()
    assert any(t['rectangles'] > 1 for traces in result['repair_traces'].values() for t in traces), 'No multi-rectangle repair plan exercised'
    assert LEVEL.read_bytes() == source, 'Saved level changed'
    result.update(passed=True, stage='complete', saved_level_unchanged=True)


try:
    flush()
    main()
except Exception:
    result['error'] = traceback.format_exc()
finally:
    try:
        if normals_path is not None and original_normals is not None:
            assert editor.EditorComponentAPIBus(bus.Broadcast, 'SetComponentProperty', component, normals_path, original_normals).IsSuccess()
            ready()
        # Restore the selector first so its guarded restoration completes before
        # restoring idle/background settings, which may suspend this Editor.
        if 'r_silpomAntiAliasing' in cvars:
            general.run_console(f"r_silpomAntiAliasing {cvars['r_silpomAntiAliasing']}")
            general.idle_wait_frames(60)
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
    except Exception:
        result['restore_error'] = traceback.format_exc()
        result['passed'] = False
    flush()
    from PySide6 import QtCore
    QtCore.QTimer.singleShot(0, general.exit_no_prompt)

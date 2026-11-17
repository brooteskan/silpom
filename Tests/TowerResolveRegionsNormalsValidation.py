"""Exact 2x sample comparisons using the timing benchmark's normals-on fixture.

Run in a dedicated Editor --runpython with processed assets and
TG_SILPOM_RESOLVE_NORMALS_OUTPUT set to a fresh directory. This script never
collects timestamps. Run test_tower_resolve_regions.py on its output afterward.
The saved level is unchanged and the temporary normals/AA/view settings are
restored. The four fixed tower views match TowerResolveRegionsBenchmark.py.
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
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.paths as paths
import azlmbr.silpom as silpom

PROJECT = Path(paths.projectroot)
LEVEL = PROJECT / 'Levels/SilPOMTowerBenchmark/SilPOMTowerBenchmark.prefab'
OUTPUT = Path(os.environ['TG_SILPOM_RESOLVE_NORMALS_OUTPUT'])
OUTPUT.mkdir(parents=True, exist_ok=True)
VIEWS = ('exterior', 'lower-facing', 'lower-glancing', 'upper-glancing')
SAMPLES = 2
VIEWPORT = (1280, 720)
result = {'passed': False, 'stage': 'startup', 'runs': [],
          'benchmark_performed': False, 'samples': SAMPLES, 'viewport': list(VIEWPORT)}
cvars = {}
initial_view = {}
component = None
normals_path = None
original_normals = None
tower = None


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


def snapshot(stem):
    records = ['Root', 'MainPipeline_0', 'DepthPrePass', 'SilPOMInspectFinal']
    colors = ['Root', 'MainPipeline_0', 'OpaquePass', 'SilPOMInspectColors']
    for suffix, slot, hierarchy in (
        ('hit', 'Records', records), ('depth', 'SampleDepth', records),
        ('diffuse', 'DiffuseSamples', colors), ('specular', 'SpecularSamples', colors),
        ('normal', 'NormalSamples', colors),
    ):
        capture(OUTPUT / f'{stem}-{suffix}.dds', slot, hierarchy)


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
    assert configuration['MaxCells'] == 65525, 'Requires tower full-screen resolve mode'
    result['level_sha256'] = hashlib.sha256(source).hexdigest()
    result['scene_sha256'] = result['level_sha256']
    assert result['scene_sha256'] == fixture['scene_sha256']
    general.idle_enable(True)
    for name, value in (('ed_keepEditorActive', 1), ('ed_backgroundUpdatePeriod', -1),
                        ('r_displayInfo', 0), ('vsync_interval', 0), ('sys_MaxFPS', -1),
                        ('r_silpomPaintBake', 0), ('r_silpomMsaaInspect', 0),
                        ('r_silpomResolveRegionTrace', 0), ('r_silpomResolveMultiRect', 1)):
        command(name, value)
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
    for name in VIEWS:
        pose = fixture['views'][name]
        result['stage'] = name + '/settling'
        flush()
        general.set_current_view_position(*map(float, pose['position']))
        general.set_current_view_rotation(*map(float, pose['rotation']))
        general.idle_wait_frames(120)
        actual = general.get_viewport_size()
        assert (int(actual.x), int(actual.y)) == VIEWPORT
        for label, enabled in (('full', 0), ('region', 1), ('control', 0)):
            stem = f'2x-{name}-{label}'
            result['stage'] = stem
            flush()
            command('r_silpomResolveRegions', enabled)
            general.idle_wait_frames(30)
            renderer_clean()
            assert aa_state() == [2, 0, 0]
            assert read(normals_path) is True
            if label == 'region':
                command('r_silpomResolveRegionTrace', 1)
                general.idle_wait_frames(2)
                command('r_silpomResolveRegionTrace', 0)
            snapshot(stem)
            result['runs'].append({'stem': stem, 'view': name, 'mode': label,
                                   'samples': SAMPLES, 'width': VIEWPORT[0], 'height': VIEWPORT[1]})
            flush()
    renderer_clean()
    log = (PROJECT / 'user/log/Editor.log').read_text(errors='replace')
    counts = [int(value) for value in re.findall(r'ResolveRegions .*?replaced=(\d+).*?indices=3', log)]
    result['traced_replacement_counts'] = counts
    assert any(value > 0 for value in counts), 'No active three-index region draw'
    rectangles = [(int(batches), int(draws)) for batches, draws in re.findall(
        r'ResolveRegions .*?replaced=(\d+) rectangles=(\d+) multi=1', log)]
    result['traced_rectangle_counts'] = rectangles
    assert any(draws > batches for batches, draws in rectangles), 'No batch used multiple rectangles'
    assert result['controls'] == assert_controls(properties, configuration)
    assert LEVEL.read_bytes() == source, 'Saved level changed'
    result.update(passed=True, stage='complete', saved_level_unchanged=True)


try:
    flush()
    main()
except Exception:
    result['error'] = traceback.format_exc()
finally:
    try:
        command('r_silpomMsaaInspect', 0)
        general.idle_wait_frames(12)
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
    except Exception:
        result['restore_error'] = traceback.format_exc()
        result['passed'] = False
    flush()
    from PySide6 import QtCore
    QtCore.QTimer.singleShot(0, general.exit_no_prompt)

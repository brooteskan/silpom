"""Unsaved per-sample A/B for conservative tower resolve regions.

Run in a dedicated source-built Editor --runpython, with processed assets and
SILPOM_RESOLVE_OUTPUT pointing to a fresh directory. Compare the output with
test_tower_resolve_regions.py. Capture work is intentionally separate from GPU
timing. No level or component property is saved or edited.

Optional SILPOM_RESOLVE_SAMPLES=1,2,4 (default 1,2),
SILPOM_RESOLVE_VIEWS=exterior,lower-facing,lower-glancing,upper-glancing,
and SILPOM_RESOLVE_ODD_SIZE=1 to repeat exterior at 1281 x 721.
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
import azlmbr.legacy.general as general
import azlmbr.paths as paths
import azlmbr.silpom as silpom

PROJECT = Path(paths.projectroot)
LEVEL = PROJECT / 'Levels/SilPOMTowerBenchmark/SilPOMTowerBenchmark.prefab'
OUTPUT = Path(os.environ['SILPOM_RESOLVE_OUTPUT'])
OUTPUT.mkdir(parents=True, exist_ok=True)
result = {'passed': False, 'stage': 'startup', 'runs': [], 'benchmark_performed': False}
cvars = {}
initial_view = {}
tower = None


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
    global tower
    source = LEVEL.read_bytes()
    fixture = json.loads(LEVEL.with_name('scene.json').read_text())
    result['level_sha256'] = hashlib.sha256(source).hexdigest()
    general.idle_enable(True)
    for name, value in (('ed_keepEditorActive', 1), ('ed_backgroundUpdatePeriod', -1),
                        ('r_displayInfo', 0), ('vsync_interval', 0), ('sys_MaxFPS', -1),
                        ('r_silpomPaintBake', 0)):
        command(name, value)
    assert general.open_level(str(LEVEL))
    general.idle_wait_frames(120)
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
    result['tower_status'] = str(silpom.SilPomMeshRequestBus(bus.Event, 'GetStatus', tower))
    assert fixture['tower_configuration']['MaxCells'] == 65525 or fixture['tower_configuration'].get('ReliefMode') == 8, 'Requires tower full-screen resolve mode'
    assert str(general.get_cvar('r_silpomResolveRegions')).lower() in ('true', 'false', '0', '1', '0.0', '1.0')
    command('r_silpomMsaaInspect', 1)
    command('r_silpomMsaaRawCoverage', 1)
    general.close_pane('Console')
    general.set_viewport_expansion_policy('FixedSize')
    samples_list = [int(value) for value in os.environ.get('SILPOM_RESOLVE_SAMPLES', '1,2').split(',')]
    names = os.environ.get('SILPOM_RESOLVE_VIEWS', 'exterior,lower-facing,lower-glancing,upper-glancing').split(',')
    views = [(name, fixture['views'][name], (1280, 720)) for name in names]
    if os.environ.get('SILPOM_RESOLVE_ODD_SIZE', '1') == '1':
        views.append(('exterior-odd', fixture['views']['exterior'], (1281, 721)))
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
            general.idle_wait_frames(120)
            actual = general.get_viewport_size()
            assert (int(actual.x), int(actual.y)) == dimensions, (name, actual)
            for label, enabled in (('full', 0), ('region', 1), ('control', 0)):
                stem = f'{samples}x-{name}-{label}'
                result['stage'] = stem
                flush()
                command('r_silpomResolveRegions', enabled)
                general.idle_wait_frames(30)
                renderer_clean()
                if label == 'region':
                    # A one-frame trace proves that this is an active region
                    # draw, rather than an equality caused by a silent fallback.
                    command('r_silpomResolveRegionTrace', 1)
                    general.idle_wait_frames(2)
                    command('r_silpomResolveRegionTrace', 0)
                snapshot(stem)
                result['runs'].append({'stem': stem, 'view': name, 'mode': label, 'samples': samples,
                                       'width': dimensions[0], 'height': dimensions[1]})
                flush()
    renderer_clean()
    log = (PROJECT / 'user/log/Editor.log').read_text(errors='replace')
    traces = re.findall(r'ResolveRegions .*?replaced=(\d+).*?indices=3', log)
    result['traced_replacement_counts'] = [int(value) for value in traces]
    assert any(int(value) > 0 for value in traces), 'No three-index region draw was observed'
    assert LEVEL.read_bytes() == source, 'Saved level changed'
    result.update(passed=True, stage='complete', saved_level_unchanged=True)


try:
    flush()
    main()
except Exception:
    result['error'] = traceback.format_exc()
finally:
    try:
        # Restore the selector first so its guarded restoration completes before
        # restoring idle/background settings, which may suspend this Editor.
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

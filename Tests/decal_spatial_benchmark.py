"""Matched gameplay paint benchmark; isolated Editor, never saves VueLiveLink.

TG_SILPOM_ESTIMATOR_OUTPUT: fresh evidence directory.
TG_PAINT_GROUPING_MODES: original (pre-change binary), or 0,1,1,0 (ABBA).
Uses saved paint and a deterministic, spatially interleaved shared-material fixture.
GPU timestamps exclude captures/rebuilds; screenshots and raw diffuse validate
compositing separately. Run with no other renderer competing for the GPU.
"""
from pathlib import Path
helper = Path(__file__).with_name('LightingEstimatorEditorSmoke.py')
exec(compile(helper.read_text().split('def main():')[0], str(helper), 'exec'))
import copy
import azlmbr.components as components
import azlmbr.math as azmath
from PySide6 import QtCore


def put(d):
    error = silpom.DecalPaintRequestBus(bus.Event, 'SetDocumentJson', canvas,
        json.dumps({'Type': 'JsonSerialization', 'Version': 1, 'ClassName': 'PaintConfig', 'ClassData': d}))
    assert not error, error


def diffuse(path, slot='DiffuseOutput'):
    done = []
    handler = atom.FrameCaptureNotificationBusHandler()
    request = atom.FrameCaptureRequestBus(bus.Broadcast, 'CapturePassAttachment', str(path),
        ['Root', 'MainPipeline_0', 'OpaquePass', 'SilPOMHitForward'], slot, 1)
    assert request.IsSuccess()
    handler.connect(request.GetValue())
    handler.add_callback('OnFrameCaptureFinished', lambda args: done.append(args[0]))
    deadline = time.monotonic() + 45
    while not done and time.monotonic() < deadline: general.idle_wait_frames(1)
    handler.disconnect()
    assert done == [atom.FrameCaptureResult_Success]


def cost(stem):
    result['benchmark_performed'] = True
    assert silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, 'BeginCapture', 180)
    deadline = time.monotonic() + 180
    while not silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, 'IsCaptureDone'):
        assert time.monotonic() < deadline
        general.idle_wait_frames(1)
    data = json.loads(silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, 'EndCapture'))
    assert data['complete'] and data['frames'] == 180
    (OUTPUT / (stem + '-gpu.json')).write_text(json.dumps(data))


def game():
    global tower, canvas
    try:
        assert general.is_in_game_mode()
        tower = general.find_game_entity('Irish Fieldstone Tower (Open Shaft)')
        canvas = general.find_game_entity(paint_name)
        ready()
        assert canvas.IsValid()
        assert silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, 'CreateReference', tower,
            'assets/silpomplanarab/irishfieldstonetowerwelded/wall/wall.azmaterial', 1, 1., 1.)
        general.idle_wait_frames(60)
        assert silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, 'IsReady')
        modes = os.environ.get('TG_PAINT_GROUPING_MODES', '0,1,1,0').split(',')
        if (OUTPUT / 'hold-for-assets').exists():
            result['stage'] = 'waiting for asset-idle marker'; flush()
            deadline = time.monotonic() + 600
            while not (OUTPUT / 'assets-ready').exists():
                assert time.monotonic() < deadline, 'Asset-idle marker timed out'
                general.idle_wait_frames(1)
        for fixture, doc in fixtures.items():
            put(doc); general.idle_wait_frames(90)
            expected = sum(len(l.get('Composite', [])) + sum(len(s.get('Points', [])) for s in l.get('Strokes', [])) for l in doc['Layers'])
            actual = sum(silpom.DecalPaintRequestBus(bus.Event, method, canvas) for method in ('GetDabCount', 'GetCompositeDabCount'))
            assert actual == expected, (fixture, actual, expected)
            for repeat, mode in enumerate(modes):
                if mode != 'original': general.run_console('r_silpomPaintSpatialGrouping ' + mode)
                general.idle_wait_frames(120)
                for name, position, rotation in views:
                    stem = f'{fixture}-{name}-{repeat}-{mode}'
                    result['stage'] = stem; flush()
                    q = azmath.Quaternion_CreateFromEulerAnglesDegrees(azmath.Vector3(*map(float, rotation)))
                    tm = azmath.Transform_CreateFromQuaternionAndTranslation(q, azmath.Vector3(*map(float, position)))
                    components.TransformBus(bus.Event, 'SetWorldTM', general.find_game_entity('Camera'), tm)
                    general.set_current_view_position(*position); general.set_current_view_rotation(*rotation)
                    general.idle_wait_frames(90)
                    actual = general.get_current_view_position()
                    assert max(abs(a-b) for a,b in zip(position, (actual.x, actual.y, actual.z))) < .01
                    cost(stem)
                    result.setdefault('runs', {})[stem] = {
                        'status': silpom.DecalPaintRequestBus(bus.Event, 'GetPaintStatus', canvas),
                        'batches': silpom.DecalPaintRequestBus(bus.Event, 'GetRenderHandleCount', canvas),
                        'msaa': str(general.get_cvar('r_multiSampleCount'))}
                    if mode != 'original':
                        result['runs'][stem]['render_stats'] = json.loads(silpom.DecalPaintRequestBus(bus.Event, 'GetRenderStatsJson', canvas))
                    if repeat < 2:
                        screenshot(OUTPUT / (stem + '.png')); diffuse(OUTPUT / (stem + '-diffuse.dds'))
                        if mode != 'original':
                            diffuse(OUTPUT / (stem + '-normal.dds'), 'NormalOutput')
                            capture_hit(OUTPUT / (stem + '-hit.dds'))
                    flush()
            if '1' in modes:
                # Same fixed geometry/view, with no paint, proves that the
                # quality comparison actually contains visible stamped pixels.
                hidden = copy.deepcopy(doc)
                for layer in hidden['Layers']: layer['Visible'] = False
                put(hidden); general.idle_wait_frames(30)
                diffuse(OUTPUT / (fixture + '-inside-no-paint-diffuse.dds'))
        result.update(passed=True, stage='complete', saved_level_unchanged=LEVEL.read_bytes() == source)
        assert result['saved_level_unchanged']
    except Exception: result['error'] = traceback.format_exc()
    finally:
        silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, 'Release')
        flush(); general.exit_game_mode(); QtCore.QTimer.singleShot(3000, general.exit_no_prompt)


try:
    source = LEVEL.read_bytes()
    result['level_sha256'] = hashlib.sha256(source).hexdigest()
    general.idle_enable(True)
    for cmd in ('ed_keepEditorActive 1', 'ed_backgroundUpdatePeriod -1', 'r_displayInfo 0',
                'vsync_interval 0', 'sys_MaxFPS -1', 'r_silpomMsaaInspect 0'):
        general.run_console(cmd)
    assert general.open_level(str(LEVEL)); general.idle_wait_frames(120)
    query = entity.SearchFilter(); query.names = ['Irish Fieldstone Tower (Open Shaft)']
    tower = entity.SearchBus(bus.Broadcast, 'SearchEntities', query)[0]; ready()
    query = entity.SearchFilter(); query.names = ['Decal Paint']
    paints = entity.SearchBus(bus.Broadcast, 'SearchEntities', query); assert len(paints) == 1
    canvas = paints[0]; paint_name = 'Decal Paint'
    saved = json.loads(silpom.DecalPaintRequestBus(bus.Event, 'GetDocumentJson', canvas))['ClassData']
    world = components.TransformBus(bus.Event, 'GetWorldTM', canvas)
    materials = []
    for layer in saved['Layers']:
        for dab in layer.get('Strokes', []) + layer.get('Composite', []):
            material = dab.get('Material')
            if material and material not in materials: materials.append(material)
    assert materials
    origin = components.TransformBus(bus.Event, 'GetWorldTranslation', tower)
    inverse = world.GetInverse()
    first_id = saved.get('NextId', 1) + 100
    layers = [{'Id': first_id+i, 'Order': [], 'Strokes': [], 'Name': 'Spatial fixture '+str(i)} for i in range(2)]
    for i in range(4096):
        # Coprime permutation defeats chronological BVH locality, as repeated
        # paint passes over different parts of a building do in normal authoring.
        k = (i * 1543) % 4096; angle = (k % 64) * math.tau / 64
        height = 1 + (k // 64) * 25 / 63; radius = 1.9 if i % 2 else 2.25
        point = inverse.Multiply(azmath.Vector3(origin.x + radius*math.cos(angle),
            origin.y + radius*math.sin(angle), origin.z + height))
        normal = inverse.TransformVector(azmath.Vector3(math.cos(angle), math.sin(angle), 0.))
        layer = layers[i // 2048]; sid = first_id+5000+i
        layer['Order'].append(sid)
        layer['Strokes'].append({'Id': sid, 'Name': 'Fixture', 'ExplicitDabs': True,
            'Material': materials[i % len(materials)], 'Diameter': .7, 'Depth': 1.2,
            'Opacity': .35, 'NormalOpacity': .15 if i % 5 == 0 else 0., 'BlendMode': i % 5,
            'Erase': i % 17 == 0, 'HeightMask': 7 if i % 13 else 5,
            'Color': [.3, .16, .08], 'PressureSize': False,
            'Points': [{'Position': [point.x, point.y, point.z], 'Normal': [normal.x, normal.y, normal.z],
                        'Start': True, 'Sequence': first_id+100+i}]})
    # Start at the current irreversible frontier, with new point ages, so the
    # controller does not repair this unrelated fixture from saved undo history.
    fixtures = {'saved': saved, 'spread': {'Layers': layers, 'NextId': first_id+10000,
        'FrozenThrough': saved.get('FrozenThrough', 0)}}
    (OUTPUT / 'fixtures.json').write_text(json.dumps(fixtures))
    result['materials'] = materials
    camera_type = editor.EditorComponentAPIBus(bus.Broadcast, 'FindComponentTypeIdsByEntityType', ['Camera'], entity.EntityType().Game)[0]
    query = entity.SearchFilter(); query.components = {camera_type: {}}
    named = entity.SearchFilter(); named.names = ['Camera']
    static = entity.SearchBus(bus.Broadcast, 'SearchEntities', named)[0]
    for e in entity.SearchBus(bus.Broadcast, 'SearchEntities', query):
        if e != static:
            c = editor.EditorComponentAPIBus(bus.Broadcast, 'GetComponentOfType', e, camera_type).GetValue()
            assert editor.EditorComponentAPIBus(bus.Broadcast, 'DisableComponents', [c])
    views = [('outside', (origin.x, origin.y-8, origin.z+3), (35., 0., 0.))]
    name, eye, direction, pose = next(camera_cases(json.loads(source)))
    a = math.radians(pose['Rotate'][2]); c, s = math.cos(a), math.sin(a)
    views.append(('inside', (pose['Translate'][0]+c*eye[0]-s*eye[1], pose['Translate'][1]+s*eye[0]+c*eye[1], pose['Translate'][2]+eye[2]),
        (math.degrees(math.atan2(direction[2], math.hypot(*direction[:2]))), 0.,
         (math.degrees(math.atan2(direction[1], direction[0]))-90+pose['Rotate'][2]+180)%360-180)))
    result['views'] = views
    general.set_viewport_expansion_policy('FixedSize'); general.set_viewport_size(1280, 720)
    general.run_console('r_silpomAntiAliasing 4'); general.idle_wait_frames(90)
    result['stage'] = 'enter gameplay'; flush()
    general.enter_game_mode(); QtCore.QTimer.singleShot(7000, game)
except Exception:
    result['error'] = traceback.format_exc(); flush(); QtCore.QTimer.singleShot(0, general.exit_no_prompt)

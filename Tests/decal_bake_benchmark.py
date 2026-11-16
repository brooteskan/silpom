"""Consolidated color/normal bake benchmark; isolated Editor, never saves VueLiveLink.

TG_SILPOM_ESTIMATOR_OUTPUT: fresh evidence directory.
TG_PAINT_BAKE_MODES: original (pre-change binary), or 0,1,1,0 (ABBA).
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

ORIGINAL_BINARY = all(mode == "original" for mode in os.environ.get("TG_PAINT_BAKE_MODES", "0,1,1,0").split(","))


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
        modes = os.environ.get('TG_PAINT_BAKE_MODES', '0,1,1,0').split(',')
        if (OUTPUT / 'hold-for-assets').exists():
            result['stage'] = 'waiting for asset-idle marker'; flush()
            deadline = time.monotonic() + 600
            while not (OUTPUT / 'assets-ready').exists():
                assert time.monotonic() < deadline, 'Asset-idle marker timed out'
                general.idle_wait_frames(1)
        selected=os.environ.get('TG_PAINT_BAKE_FIXTURES','saved,spread,patch').split(',')
        for fixture, doc in fixtures.items():
            if fixture not in selected:continue
            if not ORIGINAL_BINARY:
                general.run_console('r_silpomPaintBakeDensity '+('512' if fixture=='patch' else '128'))
            put(doc); general.idle_wait_frames(90)
            expected = sum(len(l.get('Composite', [])) + sum(len(s.get('Points', [])) for s in l.get('Strokes', [])) for l in doc['Layers'])
            actual = sum(silpom.DecalPaintRequestBus(bus.Event, method, canvas) for method in ('GetDabCount', 'GetCompositeDabCount'))
            assert actual == expected, (fixture, actual, expected)
            for repeat, mode in enumerate(modes):
                if mode != 'original': general.run_console('r_silpomPaintBake ' + mode)
                general.idle_wait_frames(120)
                if mode=='1':
                    result['stage']=fixture+'/waiting for bake';flush()
                    deadline=time.monotonic()+240
                    while True:
                        stats=json.loads(silpom.DecalPaintRequestBus(bus.Event,'GetRenderStatsJson',canvas))
                        assert not stats.get('bake_failed',0),stats
                        if stats.get('bake_pending')==0:break
                        assert time.monotonic()<deadline,stats
                        general.idle_wait_frames(1)
                    if fixture in ('saved','patch'):assert stats.get('baked_prefixes',0)>0,stats
                    result.setdefault('bakes',{})[fixture]=stats;flush()
                    general.idle_wait_frames(120)
                active_views=patch_views if fixture=='patch' else views
                for name, position, rotation in active_views:
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
                            if mode=='1':
                                general.run_console('r_silpomPaintBakeDebug 1');general.idle_wait_frames(12)
                                diffuse(OUTPUT/(stem+'-usage.dds'))
                                general.run_console('r_silpomPaintBakeDebug 2');general.idle_wait_frames(12)
                                diffuse(OUTPUT/(stem+'-reject.dds'))
                                general.run_console('r_silpomPaintBakeDebug 0');general.idle_wait_frames(12)
                    flush()
            if '1' in modes:
                # Same fixed geometry/view, with no paint, proves that the
                # quality comparison actually contains visible stamped pixels.
                hidden = copy.deepcopy(doc)
                for layer in hidden['Layers']: layer['Visible'] = False
                put(hidden); general.idle_wait_frames(30)
                for name,position,rotation in active_views:
                    q=azmath.Quaternion_CreateFromEulerAnglesDegrees(azmath.Vector3(*map(float,rotation)))
                    tm=azmath.Transform_CreateFromQuaternionAndTranslation(q,azmath.Vector3(*map(float,position)))
                    components.TransformBus(bus.Event,'SetWorldTM',general.find_game_entity('Camera'),tm)
                    general.set_current_view_position(*position);general.set_current_view_rotation(*rotation)
                    general.idle_wait_frames(30)
                    diffuse(OUTPUT/(fixture+'-'+name+'-no-paint-diffuse.dds'))
                    diffuse(OUTPUT/(fixture+'-'+name+'-no-paint-normal.dds'),'NormalOutput')
        result.update(passed=True, stage='complete', saved_level_unchanged=LEVEL.read_bytes() == source)
        assert result['saved_level_unchanged']
    except Exception: result['error'] = traceback.format_exc()
    finally:
        silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, 'Release')
        flush(); general.exit_game_mode(); QtCore.QTimer.singleShot(3000, general.exit_no_prompt)


try:
    source = LEVEL.read_bytes()
    result['level_sha256'] = hashlib.sha256(source).hexdigest()
    evidence=[Path(__file__),PROJECT/'Gems/silpom/Assets/Shaders/SilPOM/PaintBake.azsl',
        PROJECT/'user/engine-source/Gems/Atom/Feature/Common/Assets/ShaderLib/Atom/Features/PBR/DecalComposite.azsli',
        PROJECT/'build/windows/bin/profile'/os.environ.get('TG_PAINT_BAKE_MODULE','SilPOM.Editor.Bake.dll')]
    result['implementation_sha256']={str(p.relative_to(PROJECT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in evidence if p.is_file()}
    result['bake_settings']={'density':128,'patch_density':512,'budget_mib':128} if not ORIGINAL_BINARY else None

    general.idle_enable(True)
    for cmd in ('r_silpomPaintBake 0', 'ed_keepEditorActive 1', 'ed_backgroundUpdatePeriod -1', 'r_displayInfo 0',
                'vsync_interval 0', 'sys_MaxFPS -1', 'r_silpomMsaaInspect 0'):
        if not (ORIGINAL_BINARY and cmd.startswith('r_silpomPaintBake')): general.run_console(cmd)
    general.run_console('r_silpomPaintSpatialGrouping 1')
    if not ORIGINAL_BINARY: general.run_console('r_silpomPaintBakeDebug 0')
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
    # A compact, strongly overlapping normal-paint patch exercises actual
    # colored/normal cache reads, separately from broad empty-space rejection.
    local_target=inverse.Multiply(azmath.Vector3(origin.x,origin.y,origin.z+8))
    candidates=[d for l in saved['Layers'] for d in l.get('Composite',[]) if d.get('Material') and not d.get('Erase') and abs(d['Point'].get('Normal',[0,0,1])[2])<.5]
    prototype=min(candidates,key=lambda d:sum((a-b)**2 for a,b in zip(d['Point']['Position'],[local_target.x,local_target.y,local_target.z])))
    seed=prototype['Point'];normal=seed.get('Normal',[0,0,1]);length=math.sqrt(sum(v*v for v in normal));normal=[v/length for v in normal]
    axis=[0,0,1] if abs(normal[2])<.9 else [0,1,0]
    tangent=[axis[1]*normal[2]-axis[2]*normal[1],axis[2]*normal[0]-axis[0]*normal[2],axis[0]*normal[1]-axis[1]*normal[0]]
    length=math.sqrt(sum(v*v for v in tangent));tangent=[v/length for v in tangent]
    bitangent=[normal[1]*tangent[2]-normal[2]*tangent[1],normal[2]*tangent[0]-normal[0]*tangent[2],normal[0]*tangent[1]-normal[1]*tangent[0]]
    scale=world.GetUniformScale();result['paint_world_scale']=scale
    stroke={'Id':first_id+20000,'Name':'Baked normal patch','ExplicitDabs':True,'Material':prototype['Material'],
        'Diameter':.035/scale,'Depth':.04/scale,'Opacity':.85,'NormalOpacity':1.,'PressureSize':False,'Color':[.7,.45,.25],'Points':[]}
    for i in range(2048):
        k=(i*1543)%2048;u=((k%32)/31-.5)*.1/scale;v=((k//32)/63-.5)*.1/scale
        point=[seed['Position'][j]+tangent[j]*u+bitangent[j]*v for j in range(3)]
        stroke['Points'].append({'Position':point,'Normal':normal,'Sequence':first_id+13000+i,'Start':True})
    fixed=[{'Point':point,'Material':stroke['Material'],'Color':stroke['Color'],'Diameter':stroke['Diameter'],
        'Depth':stroke['Depth'],'Opacity':stroke['Opacity'],'NormalOpacity':stroke['NormalOpacity'],'StrokeId':stroke['Id']} for point in stroke['Points']]
    fixtures['patch']={'Layers':[{'Id':first_id+11000,'Order':[stroke['Id']],'Composite':fixed}],
        'NextId':first_id+30000,'FrozenThrough':first_id+15050}
    center=world.Multiply(azmath.Vector3(*seed['Position']));n=world.TransformVector(azmath.Vector3(*normal));nl=math.sqrt(n.x*n.x+n.y*n.y+n.z*n.z)
    direction=(-n.x/nl,-n.y/nl,-n.z/nl)
    patch_views=[('patch',(center.x-direction[0]*1.5,center.y-direction[1]*1.5,center.z-direction[2]*1.5),
        (math.degrees(math.atan2(direction[2],math.hypot(*direction[:2]))),0.,(math.degrees(math.atan2(direction[1],direction[0]))-90+180)%360-180))]
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

"""Unsaved 2x paint grouping integration check, with both bake paths off.

Run in a dedicated Editor --runpython with SILPOM_ESTIMATOR_OUTPUT set to a
fresh user directory and SILPOM_PAINT_GROUPING_MODES=0,1. Creates 256 deterministic
mixed dabs on an unsaved canvas, then exports exact per-sample color, normal
and hit atlases. A hidden-paint control proves visible paint. No timestamps.
"""
from pathlib import Path
helper = Path(__file__).with_name('LightingEstimatorEditorSmoke.py')
exec(compile(helper.read_text().split('def main():')[0], str(helper), 'exec'))
import copy
import azlmbr.components as components
import azlmbr.math as azmath
from PySide6 import QtCore
LEVEL=PROJECT/'Levels/SilPOMTowerBenchmark/SilPOMTowerBenchmark.prefab'
CAMERA='Camera | Exterior (Fly Camera)'


def put(d):
    error = silpom.DecalPaintRequestBus(bus.Event, 'SetDocumentJson', canvas,
        json.dumps({'Type': 'JsonSerialization', 'Version': 1, 'ClassName': 'PaintConfig', 'ClassData': d}))
    assert not error, error


def diffuse(path, slot='DiffuseOutput'):
    done = []
    handler = atom.FrameCaptureNotificationBusHandler()
    request = atom.FrameCaptureRequestBus(bus.Broadcast, 'CapturePassAttachment', str(path),
        ['Root', 'MainPipeline_0', 'OpaquePass', 'SilPOMInspectColors'],
        'NormalSamples' if slot=='NormalOutput' else 'DiffuseSamples', 1)
    assert request.IsSuccess()
    handler.connect(request.GetValue())
    handler.add_callback('OnFrameCaptureFinished', lambda args: done.append(args[0]))
    deadline = time.monotonic() + 45
    while not done and time.monotonic() < deadline: general.idle_wait_frames(1)
    handler.disconnect()
    assert done == [atom.FrameCaptureResult_Success]


def capture_hit(path):
    done=[];handler=atom.FrameCaptureNotificationBusHandler()
    response=atom.FrameCaptureRequestBus(bus.Broadcast,'CapturePassAttachment',str(path),
        ['Root','MainPipeline_0','DepthPrePass','SilPOMInspectFinal'],'Records',1)
    assert response.IsSuccess();handler.connect(response.GetValue())
    handler.add_callback('OnFrameCaptureFinished',lambda args:done.append(args[0]))
    deadline=time.monotonic()+45
    while not done and time.monotonic()<deadline:general.idle_wait_frames(1)
    handler.disconnect();assert done==[atom.FrameCaptureResult_Success]


def camera_notice_timer():
    """Acknowledge only the known camera reminder, without suppressing it."""
    from PySide6 import QtCore,QtWidgets
    settings=QtCore.QSettings()
    keys=[k for k in settings.allKeys() if k.endswith('/RestoreViewportCamera')]
    previous=settings.value(keys[0]) if keys else True
    restore=str(previous).lower() in ('true','1')
    timer=QtCore.QTimer()
    def dismiss():
        for widget in QtWidgets.QApplication.topLevelWidgets():
            if isinstance(widget,QtWidgets.QMessageBox) and 'automatically restore your camera position' in widget.text():
                if widget.checkBox():widget.checkBox().setChecked(False)
                result['camera_reminder_acknowledged_without_suppression']=True
                widget.done(QtWidgets.QMessageBox.Yes if restore else QtWidgets.QMessageBox.No)
    timer.timeout.connect(dismiss);timer.start(250)
    return timer


def game():
    global tower, canvas
    try:
        assert general.is_in_game_mode()
        tower = general.find_game_entity('Irish Fieldstone Tower (Open Shaft)')
        canvas = general.find_game_entity(paint_name)
        ready()
        result['runtime_height_derived_sampling']=True
        result['runtime_receiver_uv']=json.loads(silpom.SilPomMeshRequestBus(bus.Event,'GetReceiverUvPaintStatus',tower))
        assert result['runtime_receiver_uv']['state']=='unbound'
        assert str(general.get_cvar('r_silpomPaintBake')).lower() in ('false','0','0.0')
        assert canvas.IsValid()
        assert silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, 'CreateReference', tower,
            'assets/silpomplanarab/irishfieldstonetowerwelded/wall/wall.azmaterial', 1, 1., 1.)
        general.idle_wait_frames(60)
        assert silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, 'IsReady')
        modes = os.environ.get('SILPOM_PAINT_GROUPING_MODES', '0,1,1,0').split(',')
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
                    components.TransformBus(bus.Event, 'SetWorldTM', general.find_game_entity(CAMERA), tm)
                    general.set_current_view_position(*position); general.set_current_view_rotation(*rotation)
                    general.idle_wait_frames(90)
                    actual = general.get_current_view_position()
                    assert max(abs(a-b) for a,b in zip(position, (actual.x, actual.y, actual.z))) < .01
                    result.setdefault('runs', {})[stem] = {
                        'status': silpom.DecalPaintRequestBus(bus.Event, 'GetPaintStatus', canvas),
                        'batches': silpom.DecalPaintRequestBus(bus.Event, 'GetRenderHandleCount', canvas),
                        'msaa': str(general.get_cvar('r_multiSampleCount'))}
                    if mode != 'original':
                        result['runs'][stem]['render_stats'] = json.loads(silpom.DecalPaintRequestBus(bus.Event, 'GetRenderStatsJson', canvas))
                        assert result['runs'][stem]['batches']>0,result['runs'][stem]
                        assert result['runs'][stem]['render_stats'].get('baked_prefixes',0)==0,result['runs'][stem]
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
        flush(); notice=camera_notice_timer(); general.exit_game_mode(); general.idle_wait(3.0); notice.stop(); flush(); QtCore.QTimer.singleShot(0, general.exit_no_prompt)


try:
    source = LEVEL.read_bytes()
    result['level_sha256'] = hashlib.sha256(source).hexdigest()
    general.idle_enable(True)
    for cmd in ('ed_keepEditorActive 1', 'ed_backgroundUpdatePeriod -1', 'r_displayInfo 0',
                'vsync_interval 0', 'sys_MaxFPS -1', 'r_silpomMsaaInspect 1', 'r_silpomPaintBake 0'):
        general.run_console(cmd)
    assert general.open_level(str(LEVEL)); general.idle_wait_frames(120)
    query = entity.SearchFilter(); query.names = ['Irish Fieldstone Tower (Open Shaft)']
    tower = entity.SearchBus(bus.Broadcast, 'SearchEntities', query)[0]; ready()
    kind=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['SilPOM Mesh'],entity.EntityType().Game)[0]
    component=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentOfType',tower,kind).GetValue()
    props=editor.EditorComponentAPIBus(bus.Broadcast,'BuildComponentPropertyList',component)
    lighting_path=next(x for x in props if x.endswith('|Lighting estimator'))
    assert editor.EditorComponentAPIBus(bus.Broadcast,'SetComponentProperty',component,lighting_path,5).IsSuccess()
    assert editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentProperty',component,lighting_path).GetValue()==5
    result['lighting_estimator']=5
    normal_path=next(x for x in props if x.endswith('|Height-derived normals (off = cheaper)'))
    assert editor.EditorComponentAPIBus(bus.Broadcast,'SetComponentProperty',component,normal_path,True).IsSuccess()
    assert editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentProperty',component,normal_path).GetValue() is True
    result['height_derived_sampling']=True
    result['paint_bake_off']=str(general.get_cvar('r_silpomPaintBake'))
    assert result['paint_bake_off'].lower() in ('false','0','0.0')
    canvas=editor.ToolsApplicationRequestBus(bus.Broadcast,'CreateNewEntity',entity.EntityId())
    assert canvas.IsValid()
    paint_name='Integration Diagnostic Paint'
    editor.EditorEntityAPIBus(bus.Event,'SetName',canvas,paint_name)
    types=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['Surface Decal Paint'],entity.EntityType().Game)
    assert len(types)==1
    added=editor.EditorComponentAPIBus(bus.Broadcast,'AddComponentsOfType',canvas,types)
    assert added.IsSuccess(),str(added.GetError())
    components.TransformBus(bus.Event,'SetWorldTM',canvas,azmath.Transform_CreateIdentity())
    saved={'NextId':1,'FrozenThrough':0,'Layers':[]}
    world=components.TransformBus(bus.Event,'GetWorldTM',canvas)
    import sqlite3,uuid,azlmbr.asset as asset
    db=sqlite3.connect((PROJECT/'Cache/assetdb.sqlite').as_uri()+'?mode=ro',uri=True)
    path='materials/decalpaint/hard_round.azmaterial'
    material_id=asset.AssetCatalogRequestBus(bus.Broadcast,'GetAssetIdByPath',path,azmath.Uuid(),False)
    assert material_id.is_valid(),path
    guid,sub=db.execute('SELECT s.SourceGuid,p.SubID FROM Products p JOIN Jobs j ON j.JobID=p.JobPK JOIN Sources s ON s.SourceID=j.SourcePK WHERE p.ProductName=?',('pc/'+path,)).fetchone()
    materials=[{'assetId':{'guid':'{'+str(uuid.UUID(bytes=guid)).upper()+'}','subId':sub},'assetHint':path}]
    result['synthetic_material_catalog_id']=str(material_id)
    db.close()
    origin = components.TransformBus(bus.Event, 'GetWorldTranslation', tower)
    inverse = world.GetInverse()
    first_id = saved.get('NextId', 1) + 100
    layers = [{'Id': first_id+i, 'Order': [], 'Strokes': [], 'Name': 'Spatial fixture '+str(i)} for i in range(2)]
    for i in range(256):
        # Coprime permutation defeats chronological BVH locality, as repeated
        # paint passes over different parts of a building do in normal authoring.
        k = (i * 157) % 256; angle = (k % 16) * math.tau / 16
        height = 1 + (k // 16) * 25 / 15; radius = 1.9 if i % 2 else 2.25
        point = inverse.Multiply(azmath.Vector3(origin.x + radius*math.cos(angle),
            origin.y + radius*math.sin(angle), origin.z + height))
        facing = -1.0 if i % 2 else 1.0
        normal = inverse.TransformVector(azmath.Vector3(facing*math.cos(angle), facing*math.sin(angle), 0.))
        layer = layers[i // 128]; sid = first_id+5000+i
        layer['Order'].append(sid)
        layer['Strokes'].append({'Id': sid, 'Name': 'Fixture', 'ExplicitDabs': True,
            'Material': materials[i % len(materials)], 'Diameter': 2.0, 'Depth': 1.2,
            'Opacity': .35, 'NormalOpacity': .15 if i % 5 == 0 else 0., 'BlendMode': i % 5,
            'Erase': i % 17 == 0, 'HeightMask': 7 if i % 13 else 5,
            'Color': [.3, .16, .08], 'PressureSize': False,
            'Points': [{'Position': [point.x, point.y, point.z], 'Normal': [normal.x, normal.y, normal.z],
                        'Start': True, 'Sequence': first_id+100+i}]})
    # Start at the current irreversible frontier, with new point ages, so the
    # controller does not repair this unrelated fixture from saved undo history.
    fixtures = {'synthetic': dict(saved,Layers=layers,NextId=first_id+20000)}
    (OUTPUT / 'fixtures.json').write_text(json.dumps(fixtures))
    result['materials'] = materials
    camera_type = editor.EditorComponentAPIBus(bus.Broadcast, 'FindComponentTypeIdsByEntityType', ['Camera'], entity.EntityType().Game)[0]
    query = entity.SearchFilter(); query.components = {camera_type: {}}
    cameras=list(entity.SearchBus(bus.Broadcast,'SearchEntities',query))
    names={str(e):editor.EditorEntityInfoRequestBus(bus.Event,'GetName',e) for e in cameras}
    result['camera_names']=names
    static=next(e for e in cameras if names[str(e)]==CAMERA)
    for e in cameras:
        if e != static:
            c = editor.EditorComponentAPIBus(bus.Broadcast, 'GetComponentOfType', e, camera_type).GetValue()
            assert editor.EditorComponentAPIBus(bus.Broadcast, 'DisableComponents', [c])
    fixture=json.loads(LEVEL.with_name('scene.json').read_text())
    views=[(name,tuple(fixture['views'][view]['position']),tuple(fixture['views'][view]['rotation']))
        for name,view in [('outside','exterior'),('inside','lower-glancing')]]
    result['views'] = views
    general.set_viewport_expansion_policy('FixedSize'); general.set_viewport_size(1280, 720)
    assert int(general.get_cvar('r_silpomAntiAliasing'))==0; general.idle_wait_frames(90)
    result['stage'] = 'enter gameplay'; flush()
    general.enter_game_mode(); QtCore.QTimer.singleShot(7000, game)
except Exception:
    result['error'] = traceback.format_exc(); flush(); QtCore.QTimer.singleShot(0, general.exit_no_prompt)

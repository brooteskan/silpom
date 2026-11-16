"""Isolated receiver-first tower proof; never saves the level.

Run with the rebuilt SilPOM.Editor.dll and matching shader assets. Samples are
created only after querying an existing runtime receiver's canonical faces.
No world-space paint cache exists.
"""
from pathlib import Path
helper=Path(__file__).with_name('LightingEstimatorEditorSmoke.py')
exec(compile(helper.read_text().split('def main():')[0],str(helper),'exec'))
import copy
import azlmbr.components as components
import azlmbr.math as azmath
from PySide6 import QtCore
FIXED_COUNT=int(os.environ.get('TG_RECEIVER_UV_FIXED','2048'))
RESOLUTION=int(os.environ.get('TG_RECEIVER_UV_RESOLUTION','512'))


def sub(a,b):return tuple(x-y for x,y in zip(a,b))
def add(a,b):return tuple(x+y for x,y in zip(a,b))
def mul(a,s):return tuple(x*s for x in a)
def dot(a,b):return sum(x*y for x,y in zip(a,b))
def cross(a,b):return (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])
def length(a):return math.sqrt(dot(a,a))
def unit(a):return mul(a,1/length(a))


def put(document):
    error=silpom.DecalPaintRequestBus(bus.Event,'SetDocumentJson',canvas,json.dumps(
        {'Type':'JsonSerialization','Version':1,'ClassName':'PaintConfig','ClassData':document}))
    assert not error,error


def capture(path,pass_name,slot):
    done=[];handler=atom.FrameCaptureNotificationBusHandler()
    request=atom.FrameCaptureRequestBus(bus.Broadcast,'CapturePassAttachment',str(path),
        ['Root','MainPipeline_0','DepthPrePass' if pass_name=='SilPOMInspectFinal' else 'OpaquePass',pass_name],slot,1)
    assert request.IsSuccess(),(pass_name,slot,str(request.GetError()))
    handler.connect(request.GetValue());handler.add_callback('OnFrameCaptureFinished',lambda a:done.append(a[0]))
    try:
        deadline=time.monotonic()+60
        while not done and time.monotonic()<deadline:general.idle_wait_frames(1)
        assert done==[atom.FrameCaptureResult_Success],(path,done)
    finally:handler.disconnect()


def cost(stem):
    general.run_console('r_silpomMsaaInspect 0');general.idle_wait_frames(90)
    assert silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'BeginCapture',180)
    deadline=time.monotonic()+180
    while not silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'IsCaptureDone'):
        assert time.monotonic()<deadline;general.idle_wait_frames(1)
    data=json.loads(silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'EndCapture'))
    assert data['complete'] and data['frames']==180
    (OUTPUT/(stem+'-gpu.json')).write_text(json.dumps(data))
    result['benchmark_performed']=True


def camera(center,normal,distance,minified=False):
    import importlib
    camera_bus=None
    for module in ['azlmbr.camera','azlmbr.globals']:
        try:camera_bus=getattr(importlib.import_module(module),'CameraRequestBus',None)
        except ImportError:pass
        if camera_bus:break
    camera_entity=general.find_game_entity('Camera')
    if camera_bus:camera_bus(bus.Event,'SetFovDegrees',camera_entity,140. if minified else 60.)
    elif minified:distance=2.5
    position=add(center,mul(normal,distance));direction=mul(normal,-1)
    rotation=(math.degrees(math.atan2(direction[2],math.hypot(*direction[:2]))),0.,
        (math.degrees(math.atan2(direction[1],direction[0]))-90+180)%360-180)
    q=azmath.Quaternion_CreateFromEulerAnglesDegrees(azmath.Vector3(*map(float,rotation)))
    tm=azmath.Transform_CreateFromQuaternionAndTranslation(q,azmath.Vector3(*map(float,position)))
    components.TransformBus(bus.Event,'SetWorldTM',general.find_game_entity('Camera'),tm)
    general.set_current_view_position(*position);general.set_current_view_rotation(*rotation)
    general.idle_wait_frames(60)


def evidence(stem):
    general.run_console('r_silpomMsaaInspect 1');general.idle_wait_frames(45)
    # These atlases export sample 0/1 bit-exactly into x*4+sample, without a
    # float conversion or a generic MSAA attachment resolve.
    for p,s,label in [('SilPOMInspectFinal','Records','records'),
        ('SilPOMInspectFinal','SampleDepth','depth'),('SilPOMInspectUvUsage','Records','usage'),
        ('SilPOMInspectColors','DiffuseSamples','diffuse-samples'),
        ('SilPOMInspectColors','NormalSamples','normal-samples')]:
        capture(OUTPUT/(stem+'-'+label+'.dds'),p,s)
    screenshot(OUTPUT/(stem+'.png'))
    general.run_console('r_silpomMsaaInspect 0');general.idle_wait_frames(30)


def game():
    global tower,canvas
    try:
        assert general.is_in_game_mode()
        tower=general.find_game_entity('Irish Fieldstone Tower (Open Shaft)');canvas=general.find_game_entity('Decal Paint');ready()
        assert canvas.IsValid()
        faces=json.loads(silpom.SilPomMeshRequestBus(bus.Event,'GetPaintFacesJson',tower));assert faces
        origin=components.TransformBus(bus.Event,'GetWorldTranslation',tower);o=(origin.x,origin.y,origin.z)
        candidates=[]
        for f in faces:
            p=[c['position'] for c in f['corners']];n=unit(cross(sub(p[1],p[0]),sub(p[2],p[0])))
            center=mul(add(add(p[0],p[1]),p[2]),1/3);radial=sub(center,o)
            area=.5*length(cross(sub(p[1],p[0]),sub(p[2],p[0])))
            if f['profile']==1 and area>.5 and abs(n[2])<.05 and dot(n,(radial[0],radial[1],0))>0:
                candidates.append((abs(center[2]-o[2]-8),f,center,n))
        assert candidates
        _,face,center,normal=min(candidates,key=lambda x:x[0]);uvs=[c['uv'] for c in face['corners']]
        uv=mul(add(add(uvs[0],uvs[1]),uvs[2]),1/3)
        e1=sub(face['corners'][1]['position'],face['corners'][0]['position']);e2=sub(face['corners'][2]['position'],face['corners'][0]['position'])
        a=sub(uvs[1],uvs[0]);b=sub(uvs[2],uvs[0]);det=a[0]*b[1]-a[1]*b[0]
        du=mul(sub(mul(e1,b[1]),mul(e2,a[1])),1/det);dv=mul(sub(mul(e2,a[0]),mul(e1,b[0])),1/det)
        ru=.09/length(du);rv=.23/length(dv)
        def weights(q):
            c=sub(q,uvs[0]);v=(c[0]*b[1]-c[1]*b[0])/det;w=(a[0]*c[1]-a[1]*c[0])/det
            return (1-v-w,v,w)
        while min(min(weights((uv[0]+x*ru,uv[1]+y*rv))) for x in (-1,1) for y in (-1,1))<.05:ru*=.8;rv*=.8
        rectangle=(uv[0]-ru,uv[1]-rv,uv[0]+ru,uv[1]+rv)
        assert silpom.DecalPaintRequestBus(bus.Event,'BindReceiverUv',canvas,tower,face['face'],azmath.Vector4(*map(float,rectangle)),RESOLUTION)
        binding=json.loads(silpom.DecalPaintRequestBus(bus.Event,'GetDocumentJson',canvas))['ClassData']['ReceiverUv']
        world=components.TransformBus(bus.Event,'GetWorldTM',canvas);scale=world.GetUniformScale()
        epoch=max(saved.get('NextId',0),saved.get('FrozenThrough',0))+100000
        def point(i,live=False):
            k=(i*1543)%2048;u=uv[0]+((k%32)/31-.5)*ru*1.5;v=uv[1]+((k//32)/63-.5)*rv*1.5
            return {'Position':[0,0,0],'Normal':[0,0,1],'Sequence':epoch+i+(5000 if live else 0),'Start':True,
                'Surface':{'Receiver':binding['Receiver'],'Face':face['face'],'Uv':[u,v],'Front':True}}
        fixed=[]
        for i in range(FIXED_COUNT):
            fixed.append({'Point':point(i),'Material':materials[i%len(materials)],'Color':[.55,.3,.12],
                'Diameter':.10/scale,'Depth':.25/scale,'Rotation':float(i%31-15),'Opacity':.35,'NormalOpacity':.18,
                'StrokeId':9000,'HeightMask':7 if i%11 else 5})
        live={'Id':14000,'Name':'Live tail','ExplicitDabs':True,'Material':materials[0],'Color':[.2,.4,.65],
            'Diameter':.05/scale,'Depth':.25/scale,'Opacity':.2,'NormalOpacity':.12,'PressureSize':False,
            'Points':[point(i,True) for i in range(100)]}
        # Exercise real consolidation: submit editable source dabs and let the
        # controller freeze all but its normal 100-dab live tail.
        source_strokes=[]
        for i,d in enumerate(fixed):
            stroke={k:v for k,v in d.items() if k not in ('Point','StrokeId')}
            stroke.update(Id=epoch+7000+i,Name='Surface dab',ExplicitDabs=True,PressureSize=False,Points=[d['Point']])
            source_strokes.append(stroke)
        doc={'Layers':[{'Id':8000,'Name':'Receiver UV proof','Order':[s['Id'] for s in source_strokes]+[14000],
            'Strokes':source_strokes+[live]}],'NextId':epoch+10000,'FrozenThrough':epoch-1,'ReceiverUv':binding}
        if os.environ.get('TG_RECEIVER_UV_SMOKE')!='1' and os.environ.get('TG_RECEIVER_UV_BAKE_SMOKE')!='1':
            assert silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'CreateReference',tower,
                'assets/silpomplanarab/irishfieldstonetowerwelded/wall/wall.azmaterial',1,1.,1.)
            # Capture native rendering with no fixture paint before first
            # submitting any fixture stamps or building their disposable atlas.
            assert silpom.SilPomMeshRequestBus(bus.Event,'SetReceiverUvPaintMode',tower,0)
            put({'Layers':[],'NextId':epoch,'FrozenThrough':epoch-1,'ReceiverUv':binding})
            general.idle_wait_frames(60);camera(center,normal,.55);evidence('no-paint')
            result['no_paint_baseline_before_fixture']=True
        put(doc);general.idle_wait_frames(90)
        assert silpom.DecalPaintRequestBus(bus.Event,'GetCompositeDabCount',canvas)==FIXED_COUNT
        assert silpom.DecalPaintRequestBus(bus.Event,'GetDabCount',canvas)==100
        roundtrip=json.loads(silpom.DecalPaintRequestBus(bus.Event,'GetDocumentJson',canvas))['ClassData']
        assert all(d['Point']['Surface']['Face']==face['face'] for d in roundtrip['Layers'][0]['Composite'])
        doc=roundtrip
        result['surface_addresses_survived_consolidation']=True
        result.update(receiver=face,rectangle=rectangle,center=center,normal=normal,materials=materials,
            fixed=FIXED_COUNT,live=100,lookup='receiver instance + canonical face -> 2D texture; hit UV -> texel')
        (OUTPUT/'fixture.json').write_text(json.dumps(doc));flush()
        if os.environ.get('TG_RECEIVER_UV_SMOKE')=='1':
            result.update(passed=True,binding_smoke=True,stage='binding smoke complete',saved_level_unchanged=LEVEL.read_bytes()==source,
                cache=json.loads(silpom.SilPomMeshRequestBus(bus.Event,'GetReceiverUvPaintStatus',tower)))
            return
        if os.environ.get('TG_RECEIVER_UV_BAKE_SMOKE')=='1':
            camera(center,normal,.55);evidence('smoke-original')
            result['stage']='validated atlas bake';flush()
            assert silpom.SilPomMeshRequestBus(bus.Event,'SetReceiverUvPaintMode',tower,2)
            deadline=time.monotonic()+600
            while True:
                general.idle_wait_frames(5)
                state=json.loads(silpom.SilPomMeshRequestBus(bus.Event,'GetReceiverUvPaintStatus',tower))
                result['cache']=state;flush()
                if state['ready']:break
                assert time.monotonic()<deadline,state
            evidence('smoke-cache')
            result.update(passed=True,bake_smoke=True,stage='validated bake complete',saved_level_unchanged=LEVEL.read_bytes()==source,
                heightmaps_unchanged=all(hashlib.sha256(p.read_bytes()).hexdigest()==h for p,h in height_hashes.items()))
            return
        for repeat,mode in enumerate([0,1,2,2,1,0]):
            assert silpom.SilPomMeshRequestBus(bus.Event,'SetReceiverUvPaintMode',tower,mode)
            general.idle_wait_frames(60)
            if mode==2:
                deadline=time.monotonic()+600
                while True:
                    state=json.loads(silpom.SilPomMeshRequestBus(bus.Event,'GetReceiverUvPaintStatus',tower))
                    result['cache']=state;result['stage']='baking';flush()
                    if state['ready']:break
                    assert time.monotonic()<deadline,state;general.idle_wait_frames(15)
            # Stay in the shaft: moving six metres away hid this receiver
            # behind another wall. A wider FOV minifies the visible surface.
            for name,distance in [('close',.55),('ordinary',1.5),('minified',1.5)]:
                stem=f'{name}-{repeat}-{mode}';result['stage']=stem;flush();camera(center,normal,distance,name=='minified');cost(stem)
                if repeat<3:evidence(stem)
                if mode==2:
                    current=json.loads(silpom.SilPomMeshRequestBus(bus.Event,'GetReceiverUvPaintStatus',tower))
                    assert current['ready'] and current['bake_ms']==state['bake_ms'],'Camera movement rebuilt or selected a different cache'
        # Editing only the live tail must keep the fixed atlas and change paint.
        assert silpom.SilPomMeshRequestBus(bus.Event,'SetReceiverUvPaintMode',tower,2)
        modified=copy.deepcopy(doc);modified['Layers'][0]['Strokes'][0]['Color']=[.9,.05,.1]
        put(modified);general.idle_wait_frames(60)
        assert json.loads(silpom.SilPomMeshRequestBus(bus.Event,'GetReceiverUvPaintStatus',tower))['bake_ms']==state['bake_ms']
        camera(center,normal,.55);evidence('live-tail-edited');result['live_tail_keeps_fixed_atlas']=True
        tail_only=copy.deepcopy(doc);tail_only['Layers'][0]['Composite']=[]
        tail_only['Layers'][0]['Order']=[14000]
        put(tail_only);general.idle_wait_frames(60);evidence('live-tail-only')
        # Invalid surface identity must not become a volume waiting for a wall.
        orphan=copy.deepcopy(doc)
        for d in orphan['Layers'][0]['Composite']:d['Point']['Surface']['Face']='absent-canonical-face'
        for p in orphan['Layers'][0]['Strokes'][0]['Points']:p['Surface']['Face']='absent-canonical-face'
        put(orphan);general.idle_wait_frames(60);evidence('orphan-surface')
        # Clear the document: an enabled cache must not retain orphan paint.
        put({'Layers':[],'NextId':epoch+20000,'FrozenThrough':epoch+3000,'ReceiverUv':binding});general.idle_wait_frames(60)
        camera(center,normal,.55);evidence('zero-prefix')
        result.update(passed=True,stage='complete',saved_level_unchanged=LEVEL.read_bytes()==source)
        result['heightmaps_unchanged']=all(hashlib.sha256(p.read_bytes()).hexdigest()==h for p,h in height_hashes.items())
        log=(PROJECT/'user/log/Editor.log').read_text(errors='replace')
        result['no_skipped_resource_bindings']='Failed to bind shader resources for draw item' not in log
        assert result['no_skipped_resource_bindings'],'A draw was skipped for missing resource bindings; invalidate benchmark'
        assert result['saved_level_unchanged']
        assert result['heightmaps_unchanged']
    except Exception:result.update(passed=False,error=traceback.format_exc())
    finally:
        silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'Release');flush()
        general.exit_game_mode();QtCore.QTimer.singleShot(3000,general.exit_no_prompt)


try:
    source=LEVEL.read_bytes();result['level_sha256']=hashlib.sha256(source).hexdigest()
    height_hashes={p:hashlib.sha256(p.read_bytes()).hexdigest() for p in (PROJECT/'Assets/SilPOMPlanarAB').rglob('height.png')}
    result['heightmaps_before']={str(p.relative_to(PROJECT)):h for p,h in height_hashes.items()};assert height_hashes
    general.idle_enable(True)
    for cmd in ('r_silpomPaintBake 0','ed_keepEditorActive 1','ed_backgroundUpdatePeriod -1',
        'r_displayInfo 0','vsync_interval 0','sys_MaxFPS -1','r_silpomMsaaInspect 0','r_silpomAntiAliasing 4'):
        general.run_console(cmd)
    assert general.open_level(str(LEVEL));general.idle_wait_frames(120)
    query=entity.SearchFilter();query.names=['Irish Fieldstone Tower (Open Shaft)'];tower=entity.SearchBus(bus.Broadcast,'SearchEntities',query)[0];ready()
    query.names=['Decal Paint'];canvas=entity.SearchBus(bus.Broadcast,'SearchEntities',query)[0]
    saved=json.loads(silpom.DecalPaintRequestBus(bus.Event,'GetDocumentJson',canvas))['ClassData'];materials=[]
    for layer in saved['Layers']:
        for dab in layer.get('Composite',[])+layer.get('Strokes',[]):
            material=dab.get('Material')
            if material and material not in materials:materials.append(material)
    assert materials
    camera_type=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['Camera'],entity.EntityType().Game)[0]
    query=entity.SearchFilter();query.components={camera_type:{}}
    named=entity.SearchFilter();named.names=['Camera'];static=entity.SearchBus(bus.Broadcast,'SearchEntities',named)[0]
    for e in entity.SearchBus(bus.Broadcast,'SearchEntities',query):
        if e!=static:
            c=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentOfType',e,camera_type).GetValue()
            assert editor.EditorComponentAPIBus(bus.Broadcast,'DisableComponents',[c])
    general.set_viewport_expansion_policy('FixedSize');general.set_viewport_size(1280,720)
    result['stage']='enter gameplay';flush();general.enter_game_mode();QtCore.QTimer.singleShot(7000,game)
except Exception:
    result['error']=traceback.format_exc();flush();QtCore.QTimer.singleShot(0,general.exit_no_prompt)

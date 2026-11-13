"""Near-wall moving-light and diffuse-only accessibility experiments.

Captures the approved default before experiments. No source level is saved.
Run in an isolated Editor with TG_SILPOM_ESTIMATOR_OUTPUT set.
"""
from pathlib import Path
helper=Path(__file__).with_name('LightingEstimatorEditorSmoke.py')
exec(compile(helper.read_text().split('def main():')[0],str(helper),'exec'))
import azlmbr.components as components
import azlmbr.math as azmath
import struct

L='Lighting estimator';AO='Diffuse ambient accessibility (experimental)'
P='Level 5A learned relief LOD (experimental)';D='Level 5B bounded proxy depth (experimental)'
BRIGHT='Spiral light master brightness';NEAR='Neural directional visibility (off = cheaper)'
light_originals=[];sun=None;sun_transform=None;source=None

def read(comp,path):
    value=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentProperty',comp,path);assert value.IsSuccess(),path
    return value.GetValue()

def write(comp,path,value):
    result=editor.EditorComponentAPIBus(bus.Broadcast,'SetComponentProperty',comp,path,value);assert result.IsSuccess(),(path,str(result.GetError()))

def capture(path,slot):
    done=[];h=atom.FrameCaptureNotificationBusHandler()
    response=atom.FrameCaptureRequestBus(bus.Broadcast,'CapturePassAttachment',str(path),['Root','MainPipeline_0','OpaquePass','SilPOMHitForward'],slot,1)
    assert response.IsSuccess(),(slot,str(response.GetError()));h.connect(response.GetValue())
    h.add_callback('OnFrameCaptureFinished',lambda args:done.append(args[0]))
    try:
        deadline=time.monotonic()+45
        while not done and time.monotonic()<deadline:general.idle_wait_frames(1)
        assert done==[atom.FrameCaptureResult_Success],(slot,done)
    finally:h.disconnect()

def direct_lights():
    global sun,sun_transform
    q=entity.SearchFilter();entities=entity.SearchBus(bus.Broadcast,'SearchEntities',q)
    for type_name in ('Directional Light','Light'):
        types=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',[type_name],entity.EntityType().Game)
        assert types and not types[0].IsNull(),type_name
        for eid in entities:
            found=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentOfType',eid,types[0])
            if not found.IsSuccess():continue
            comp=found.GetValue();paths=editor.EditorComponentAPIBus(bus.Broadcast,'BuildComponentPropertyList',comp)
            intensity=next(p for p in paths if p.endswith('|Intensity'))
            light_originals.append((comp,intensity,read(comp,intensity)))
            write(comp,intensity,0.0)
            if type_name=='Directional Light':
                assert sun is None,'Use a single directional light for this controlled comparison'
                sun=eid;sun_transform=components.TransformBus(bus.Event,'GetWorldTM',eid)
                shadow=next(p for p in paths if p.endswith('|Enable Shadow'))
                light_originals.append((comp,shadow,read(comp,shadow)));write(comp,shadow,False)
                result['sun_intensity_property']=intensity;result['sun_component']=str(comp)
                result['sun_properties']=list(paths)
                for path in paths:
                    if 'Lighting Channels' in path:
                        value=read(comp,path)
                        if isinstance(value,bool):
                            light_originals.append((comp,path,value));write(comp,path,True)
                global sun_comp,sun_intensity
                sun_comp,sun_intensity=comp,intensity
    assert sun is not None and len(light_originals)>5,'No controlled scene lights found'
    result['direct_light_controls_disabled']=len(light_originals);flush()

def snapshot(stem):
    general.idle_wait_frames(24);screenshot(OUTPUT/(stem+'.png'))
    for slot in ('Hit','DiffuseOutput','SpecularOutput','AlbedoOutput','NormalOutput'):
        capture(OUTPUT/(stem+'-'+slot+'.dds'),slot)

def main():
    global tower,component,properties,source
    general.idle_enable(True);source=LEVEL.read_bytes();assert general.open_level(str(LEVEL));general.idle_wait_frames(90)
    position,rotation=general.get_current_view_position(),general.get_current_view_rotation();size=general.get_viewport_size()
    initial_view.update(position=(position.x,position.y,position.z),rotation=(rotation.x,rotation.y,rotation.z),
        policy=general.get_viewport_expansion_policy(),size=(int(size.x),int(size.y)))
    q=entity.SearchFilter();q.names=['Irish Fieldstone Tower (Open Shaft)']
    towers=entity.SearchBus(bus.Broadcast,'SearchEntities',q);assert len(towers)==1;tower=towers[0];ready()
    types=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['SilPOM Mesh'],entity.EntityType().Game)
    found=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentOfType',tower,types[0]);assert found.IsSuccess();component=found.GetValue()
    available=editor.EditorComponentAPIBus(bus.Broadcast,'BuildComponentPropertyList',component)
    ref=json.loads(Path(__file__).with_name('Reference').joinpath('NearestHitVisual.json').read_text())
    properties={label:next(p for p in available if p.endswith('|'+label)) for label in (L,AO,P,D,BRIGHT,NEAR)}
    paths=set(ref['tower_quality_controls'])|set(properties.values())
    for path in paths:originals[path]=read(component,path)
    result['original_lighting_estimator']=read(component,properties[L]);result['original_ambient']=read(component,properties[AO])
    # Restore the frozen reference controls only for the default proof. The
    # new candidate and AO remain excluded from this shader route.
    write(component,properties[AO],False);write(component,properties[L],0)
    for path,value in ref['tower_quality_controls'].items():write(component,path,value)
    ready();general.idle_wait_frames(30)
    result['quality_controls']={path:read(component,path) for path in ref['tower_quality_controls']}
    assert result['quality_controls']==ref['tower_quality_controls']
    for key,value in (('ed_keepEditorActive',1),('ed_backgroundUpdatePeriod',-1),('r_displayInfo',0),('r_antiAliasing',0)):
        cvars[key]=general.get_cvar(key);general.run_console(f'{key} {value}')
    general.close_pane('Console');general.set_viewport_expansion_policy('FixedSize');general.set_viewport_size(1280,720);general.update_viewport();general.idle_wait_frames(60)
    folder=OUTPUT/'current';folder.mkdir(exist_ok=True);record={'views':{}};result['variants']['current']=record
    cases=list(camera_cases(json.loads(source)))
    for name,eye,direction,pose in cases:
        result['stage']='reference/'+name;flush();record['views'][name]=view(eye,direction,pose);general.idle_wait_frames(45)
        screenshot(folder/(name+'.png'));capture_hit(folder/(name+'-hit.dds'))
        if 'glancing' in name:
            for i in range(5):
                view((eye[0]+.002*i,eye[1]+.003*i,eye[2]+.002*i),direction,pose);general.idle_wait_frames(4)
                screenshot(folder/(name+f'-motion-{i}.png'))
                if i in (1,4):capture_hit(folder/(name+f'-motion-{i}-hit.dds'))
    # Near-only comparison excludes the joint proxy visibility/reflectance
    # response. Disable all direct scene lights for the ambient experiment.
    set_value(P,False);set_value(L,5);set_value(AO,False);set_value(BRIGHT,0.0);set_value(NEAR,True)
    direct_lights();name,eye,direction,pose=cases[0];view(eye,direction,pose);general.idle_wait_frames(45)
    result['stage']='environment only';flush();snapshot('environment-off');set_value(AO,True);snapshot('environment-on')
    set_value(AO,False);snapshot('environment-restored')
    # A slow directional sweep, identical material/geometry/light controls for
    # each model. Capture direct HDR separately from environment contributions.
    write(sun_comp,sun_intensity,25.0)
    # Illuminate the camera-facing receiver, then sweep around that direction.
    angle=math.atan2(-direction[1],-direction[0])+math.radians(pose['Rotate'][2])
    result['moving_light_angles']=[]
    for mode in (3,5):
        set_value(L,mode)
        for i in range(13):
            yaw=angle+math.radians(-18+i*3);elevation=math.radians(12)
            to_light=(math.cos(yaw)*math.cos(elevation),math.sin(yaw)*math.cos(elevation),math.sin(elevation))
            pitch=math.asin(-to_light[2]);rotation=math.atan2(to_light[0],-to_light[1])
            q=azmath.Quaternion_CreateFromEulerRadiansXYZ(azmath.Vector3(float(pitch),0.0,float(rotation)))
            components.TransformBus(bus.Event,'SetWorldRotationQuaternion',sun,q);general.idle_wait_frames(8)
            actual=components.TransformBus(bus.Event,'GetWorldRotationQuaternion',sun)
            assert abs(actual.Dot(q))>.9999,'Directional light rotation did not update'
            result['stage']=f'moving/{mode}/{i}';flush()
            capture(OUTPUT/f'moving-{mode}-{i:02d}-Hit.dds','Hit');capture(OUTPUT/f'moving-{mode}-{i:02d}-DiffuseOutput.dds','DiffuseOutput')
            screenshot(OUTPUT/f'moving-{mode}-{i:02d}.png')
            if mode==3:result['moving_light_angles'].append(to_light)
    # Ambient on/off must leave direct HDR and all specular terms alone, even
    # with a bright direct light. Albedo RGB alone carries diffuse accessibility.
    set_value(L,5);set_value(AO,False);snapshot('direct-off');set_value(AO,True);snapshot('direct-on')
    set_value(AO,False);snapshot('direct-restored')
    assert LEVEL.read_bytes()==source
    result.update(passed=True,stage='complete',saved_level_unchanged=True,near_only=True)

try:
    flush();main()
except Exception:result['error']=traceback.format_exc()
finally:
    try:
        if sun is not None and sun_transform is not None:components.TransformBus(bus.Event,'SetWorldTM',sun,sun_transform)
        for comp,path,value in reversed(light_originals):write(comp,path,value)
        if component is not None:
            # Clear dependants before restoring the original component values.
            write(component,properties[AO],False)
            for path,value in originals.items():write(component,path,value)
            ready()
        for key,value in cvars.items():general.run_console(f'{key} {value}')
        if initial_view:
            general.set_current_view_position(*initial_view['position']);general.set_current_view_rotation(*initial_view['rotation'])
            if general.get_viewport_expansion_policy()!=initial_view['policy']:
                general.set_viewport_expansion_policy(initial_view['policy']);general.idle_wait_frames(20)
            if initial_view['policy']=='FixedSize':general.set_viewport_size(*initial_view['size']);general.idle_wait_frames(20)
        result['restored']=True
    except Exception:result['restore_error']=traceback.format_exc();result['passed']=False
    flush()
    from PySide6 import QtCore
    QtCore.QTimer.singleShot(0,general.exit_no_prompt)

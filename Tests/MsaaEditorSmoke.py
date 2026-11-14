"""Isolated AA/4x sample-ownership comparison; never saves the level."""
from pathlib import Path
helper=Path(__file__).with_name('AngularEdgesEditorSmoke.py')
exec(compile(helper.read_text().split('\ndef main():')[0],str(helper),'exec'))
from PySide6 import QtGui
original_smaa=None


def renderer_clean():
    log=(PROJECT/'user/log/Editor.log').read_text(errors='replace')
    assert not any(s in log for s in ('Failed to bind shader resources for draw item',
        "[Error] (DeviceShaderResourceGroupData)", 'Trace::Assert')), 'Renderer binding/assert error; stop the comparison'


def reference_image(path,reference):
    image=QtGui.QImage(str(path)).convertToFormat(QtGui.QImage.Format.Format_RGBA8888)
    assert not image.isNull()
    assert hashlib.sha256(bytes(image.constBits())).hexdigest()==reference['captures'][path.name]['sha256'], \
        'The 1x image does not match the approved lit reference; stop before further AA tests'


def command(name,value):
    if name not in cvars:cvars[name]=general.get_cvar(name)
    general.run_console(f'{name} {value}')


def inspect(stem):
    for pass_name in ('SilPOMInspectRaw','SilPOMInspectFinal'):
        for slot in ('Records','SampleDepth'):
            capture(OUTPUT/f'{stem}-{pass_name}-{slot}.dds',slot,
                ['Root','MainPipeline_0','DepthPrePass',pass_name])
    for slot in ('DiffuseSamples','SpecularSamples'):
        capture(OUTPUT/f'{stem}-{slot}.dds',slot,['Root','MainPipeline_0','OpaquePass','SilPOMInspectColors'])
    capture(OUTPUT/f'{stem}-ResolvedDiffuse.dds','Output',
        ['Root','MainPipeline_0','OpaquePass','MSAAResolveDiffusePass'])


def gpu_cost(mode):
    # Timestamp collection only: no screenshots, readbacks, inspection atlases
    # or active shader compilation during these steady-state frames.
    command('r_silpomMsaaInspect',0);general.idle_wait_frames(120);renderer_clean()
    assert silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'CreateReference',tower,
        'assets/silpomplanarab/irishfieldstonetowerwelded/wall/wall.azmaterial',1,1.0,1.0)
    general.idle_wait_frames(30)
    assert silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'IsReady')
    started=time.monotonic()
    assert silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'BeginCapture',120)
    deadline=time.monotonic()+120
    while not silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'IsCaptureDone') and time.monotonic()<deadline:
        general.idle_wait_frames(1)
    data=json.loads(silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'EndCapture'))
    data['editor_capture_wall_seconds']=time.monotonic()-started
    silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'Release')
    assert data['complete'];(OUTPUT/f'gpu-{mode}.json').write_text(json.dumps(data,indent=2))


def main():
    global tower,component,properties,source,original_smaa
    general.idle_enable(True);source=LEVEL.read_bytes();result['level_sha256']=hashlib.sha256(source).hexdigest()
    assert general.open_level(str(LEVEL));general.idle_wait_frames(90)
    position,rotation=general.get_current_view_position(),general.get_current_view_rotation();size=general.get_viewport_size()
    initial_view.update(position=(position.x,position.y,position.z),rotation=(rotation.x,rotation.y,rotation.z),
        policy=general.get_viewport_expansion_policy(),size=(int(size.x),int(size.y)))
    q=entity.SearchFilter();q.names=['Irish Fieldstone Tower (Open Shaft)']
    towers=entity.SearchBus(bus.Broadcast,'SearchEntities',q);assert len(towers)==1;tower=towers[0];ready()
    original_smaa=silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'GetSmaaEnabled',tower)
    assert silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'SetSmaaEnabled',tower,False)
    types=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['SilPOM Mesh'],entity.EntityType().Game)
    found=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentOfType',tower,types[0]);assert found.IsSuccess();component=found.GetValue()
    available=editor.EditorComponentAPIBus(bus.Broadcast,'BuildComponentPropertyList',component)
    ref=json.loads(Path(__file__).with_name('Reference').joinpath('NearestHitVisual.json').read_text())
    properties={label:next(p for p in available if p.endswith('|'+label)) for label in (L,AO,P,D,BRIGHT,NEAR)}
    for path in set(ref['tower_quality_controls'])|set(properties.values()):originals[path]=read(component,path)
    result['saved_tower_controls']=dict(originals)
    command('r_silpomMsaa4',0);command('r_silpomMsaaRawCoverage',1);command('r_silpomMsaaInspect',0)
    command('r_multiSampleCount',1);command('r_antiAliasing','MSAA')
    for name,value in (('ed_keepEditorActive',1),('ed_backgroundUpdatePeriod',-1),('r_displayInfo',0),('vsync_interval',0),('sys_MaxFPS',-1)):command(name,value)
    general.close_pane('Console');general.set_viewport_expansion_policy('FixedSize');general.set_viewport_size(1280,720)
    general.update_viewport();general.idle_wait_frames(60)
    cases=list(camera_cases(json.loads(source)))
    name,eye,direction,pose=cases[0];view(eye,direction,pose)
    result['stage']='saved tower GPU baseline';flush();gpu_cost('saved-1x')
    write(component,properties[AO],False);write(component,properties[L],0)
    for path,value in ref['tower_quality_controls'].items():write(component,path,value)
    ready();general.idle_wait_frames(30)
    result['quality_controls']={p:read(component,p) for p in ref['tower_quality_controls']}
    folder=OUTPUT/'current';folder.mkdir(exist_ok=True);record={'views':{}};result['variants']['current']=record
    for name,eye,direction,pose in cases:
        result['stage']='reference/'+name;flush();record['views'][name]=view(eye,direction,pose);general.idle_wait_frames(45)
        renderer_clean();screenshot(folder/(name+'.png'));reference_image(folder/(name+'.png'),ref)
        capture_hit(folder/(name+'-hit.dds'))
        if 'glancing' in name:
            for i in range(5):
                view((eye[0]+.002*i,eye[1]+.003*i,eye[2]+.002*i),direction,pose);general.idle_wait_frames(4)
                screenshot(folder/(name+f'-motion-{i}.png'))
                if i in (1,4):capture_hit(folder/(name+f'-motion-{i}-hit.dds'))
    # No resolution-dependent representation selection in any AA comparison.
    set_value(P,False);set_value(L,0);set_value(AO,False)
    result['representation']='Level 5A/5B disabled; fixed world-space near/shell cutoff, geometry and models'
    for mode in ('none','SMAA','TAA','supersample2x','msaa4'):
        result['stage']=mode;flush()
        renderer_clean()
        assert silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'SetSmaaEnabled',tower,mode=='SMAA')
        # Atom accepts mode names. "0" is rejected and can leave TAA's jitter
        # active after its comparison, invalidating per-sample A/B readbacks.
        command('r_antiAliasing',mode if mode in ('SMAA','TAA') else 'MSAA')
        general.set_viewport_size(*( (2560,1440) if mode=='supersample2x' else (1280,720) ))
        if mode=='msaa4':
            command('r_silpomMsaa4',1);general.idle_wait_frames(120)
            assert str(general.get_cvar('r_silpomMsaa4')).lower() in ('true','1'),'4x capability check rejected the device'
        for name,eye,direction,pose in cases:
            view(eye,direction,pose);general.idle_wait_frames(90)
            screenshot(OUTPUT/f'{mode}-{name}.png')
            if mode=='msaa4':
                command('r_silpomMsaaInspect',1);general.idle_wait_frames(12);inspect(name+'-raw')
                command('r_silpomMsaaRawCoverage',0);general.idle_wait_frames(12);inspect(name+'-repaired')
                screenshot(OUTPUT/f'msaa4-repaired-{name}.png')
                command('r_silpomMsaaRawCoverage',1);command('r_silpomMsaaInspect',0)
            if name==cases[0][0] and mode in ('none','SMAA','TAA','msaa4'):gpu_cost(mode)
            for i in range(8):
                view((eye[0]+.001*i,eye[1]+.0015*i,eye[2]+.001*i),direction,pose)
                general.idle_wait_frames(2);screenshot(OUTPUT/f'{mode}-{name}-motion-{i}.png')
    result['stage']='saved tower at msaa4';flush()
    comparison_controls={path:read(component,path) for path in originals}
    write(component,properties[AO],False)
    for path,value in originals.items():write(component,path,value)
    ready();assert all(read(component,p)==v for p,v in originals.items())
    name,eye,direction,pose=cases[0];view(eye,direction,pose)
    command('r_silpomMsaaInspect',1);general.idle_wait_frames(45);renderer_clean()
    screenshot(OUTPUT/'saved-msaa4.png');inspect('saved-msaa4');gpu_cost('saved-msaa4')
    write(component,properties[AO],False)
    for path,value in comparison_controls.items():write(component,path,value)
    ready()
    result['stage']='msaa4 proxy depth writers';flush()
    set_value(P,True);command('r_silpomMsaaInspect',1)
    result['proxy_cases']={}
    for learned_depth in (False,True):
        set_value(D,learned_depth);view((60,0,12),(-1,0,0),pose);general.idle_wait_frames(60);renderer_clean()
        stem='proxy-depth' if learned_depth else 'proxy-plane'
        owners=list(silpom.SilPomMeshRequestBus(bus.Event,'GetProxyFaceOwners',tower));assert owners
        result['proxy_cases'][stem]={'owners':owners};inspect(stem);screenshot(OUTPUT/(stem+'.png'))
    set_value(D,False);set_value(P,False)
    result['stage']='msaa4 indirect lighting';flush()
    set_value(BRIGHT,0.0);direct_lights()
    name,eye,direction,pose=cases[0];view(eye,direction,pose)
    command('r_silpomMsaaInspect',1);general.idle_wait_frames(30);inspect('environment-dark')
    indirect_fixture();general.idle_wait_frames(60);renderer_clean()
    screenshot(OUTPUT/'environment-lit.png');inspect('environment-lit')
    restore_indirect()
    for comp,path,value in reversed(light_originals):write(comp,path,value)
    light_originals.clear()
    command('r_silpomMsaaInspect',0)
    command('r_silpomMsaa4',0);general.idle_wait_frames(90)
    assert LEVEL.read_bytes()==source
    result.update(passed=True,stage='complete',saved_level_unchanged=True)


try:
    flush();main()
except Exception:result['error']=traceback.format_exc()
finally:
    try:
        silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'Release')
        if original_smaa is not None:
            assert silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'SetSmaaEnabled',tower,original_smaa)
        if indirect_originals or disabled_grids or environment_originals:restore_indirect()
        for comp,path,value in reversed(light_originals):write(comp,path,value)
        general.run_console('r_silpomMsaa4 0');general.run_console('r_silpomMsaaInspect 0');general.idle_wait_frames(60)
        if component is not None:
            write(component,properties[AO],False)
            for path,value in originals.items():write(component,path,value)
            ready()
        for key,value in cvars.items():general.run_console(f'{key} {value}')
        if initial_view:
            general.set_current_view_position(*initial_view['position']);general.set_current_view_rotation(*initial_view['rotation'])
            general.set_viewport_expansion_policy(initial_view['policy'])
            if initial_view['policy']=='FixedSize':general.set_viewport_size(*initial_view['size'])
            general.idle_wait_frames(30)
        result['restored']=True
    except Exception:result['restore_error']=traceback.format_exc();result['passed']=False
    flush()
    from PySide6 import QtCore
    QtCore.QTimer.singleShot(0,general.exit_no_prompt)

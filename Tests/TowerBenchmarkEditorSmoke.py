"""Validate the portable lit tower scene; optional GPU timestamps, no level saves."""
from pathlib import Path
helper=Path(__file__).with_name('MsaaEditorSmoke.py')
exec(compile(helper.read_text().split('\ndef main():')[0],str(helper),'exec'))
LEVEL=PROJECT/'Levels/SilPOMTowerBenchmark/SilPOMTowerBenchmark.prefab'


def main():
    global tower,component
    timing_only=os.environ.get('TG_SILPOM_TOWER_TIMING_ONLY','0')=='1'
    timing=os.environ.get('TG_SILPOM_MSAA_TIMING','0')=='1'
    renderdoc_view=os.environ.get('TG_SILPOM_TOWER_RENDERDOC_VIEW','')
    assert not renderdoc_view or not timing,'RenderDoc inspection must be separate from timing'
    repeats=int(os.environ.get('TG_SILPOM_TOWER_TIMING_REPEATS','3'))
    assert not timing_only or timing,'Timing-only mode requires TG_SILPOM_MSAA_TIMING=1'
    assert not timing or repeats>=3,'Use at least three captures per camera'
    result.update(timing_only=timing_only,timing_repeats=repeats if timing else 0)
    source=LEVEL.read_bytes()
    original=(PROJECT/'Levels/VueLiveLink/VueLiveLink.prefab').read_bytes()
    fixture=json.loads(LEVEL.with_name('scene.json').read_text())
    assert not renderdoc_view or renderdoc_view in fixture['views'],renderdoc_view
    assert hashlib.sha256(source).hexdigest()==fixture['scene_sha256']
    general.idle_enable(True)
    for name,value in (('ed_keepEditorActive',1),('ed_backgroundUpdatePeriod',-1),('r_displayInfo',0),
                       ('vsync_interval',0),('sys_MaxFPS',-1)):
        command(name,value)
    assert general.open_level(str(LEVEL));general.idle_wait_frames(90)
    query=entity.SearchFilter();query.names=['Irish Fieldstone Tower (Open Shaft)']
    found=entity.SearchBus(bus.Broadcast,'SearchEntities',query);assert len(found)==1;tower=found[0]
    kind=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['SilPOM Mesh'],entity.EntityType().Game)[0]
    component=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentOfType',tower,kind).GetValue()
    props=editor.EditorComponentAPIBus(bus.Broadcast,'BuildComponentPropertyList',component)
    cfg=fixture['tower_configuration']
    state=silpom.SilPomMeshRequestBus(bus.Event,'GetAntiAliasingState',tower)
    result['initial_aa_state']=[int(state.x),int(state.y),int(state.z)]
    samples=int(os.environ.get('TG_SILPOM_TOWER_SAMPLES',str(int(state.x))))
    assert samples in (1,2),samples
    result['samples']=samples
    result['initial_renderer_cvars']={name:str(general.get_cvar(name)) for name in
        ('r_multiSampleCount','r_antiAliasing','r_silpomAntiAliasing','r_silpomReuseDepthHits','r_silpomMsaa4')}
    result['initial_tower_status']=str(silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',tower))
    result['initial_profile_controls']={p:str(read(component,p)) for p in props
        if p.endswith('|Neural model') or 'Relief depth multiplier' in p}
    result['stage']='configured normals-on; waiting for tower';flush()
    # User-requested baseline correction: face normals expose seams at mesh edges.
    normals=next(p for p in props if p.endswith('|Height-derived normals (off = cheaper)'))
    result['requested_overrides']={'UseHeightDerivedNormals':True}
    result['saved_height_derived_normals']=cfg.get('UseHeightDerivedNormals',False)
    effective_cfg=dict(cfg);effective_cfg['UseHeightDerivedNormals']=True
    result['tower_configuration_sha256']=fixture['tower_configuration_sha256']
    result['effective_tower_configuration_sha256']=hashlib.sha256(json.dumps(effective_cfg,sort_keys=True).encode()).hexdigest()
    if not read(component,normals):
        write(component,normals,True)
    ready()
    expected={'Lighting estimator':5,
        'Height-derived normals (off = cheaper)':True,
        'Level 2 predictor (experimental)':cfg['UseLevelTwoPredictor'],
        'Gather validation (may be cheaper)':cfg['UseValidationGather'],
        'R16 height textures (less GPU memory)':cfg['UseR16HeightTextures']}
    result['controls']={}
    for label,value in expected.items():
        path=next(p for p in props if p.endswith('|'+label))
        actual=read(component,path);assert actual==value,(label,actual,value)
        result['controls'][label]=actual
    aa_path=next((p for p in props if p.endswith('|Anti-aliasing')),None)
    if aa_path:
        assert read(component,aa_path)==0
        result['aa_control_scope']='legacy component'
    else:
        assert int(general.get_cvar('r_silpomAntiAliasing'))==0
        result['aa_control_scope']='renderer-wide project default'
    result['controls']['Anti-aliasing']=0
    result['tower_status']=str(silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',tower))
    assert [int(x) for x in (lambda v:(v.x,v.y,v.z))(silpom.SilPomMeshRequestBus(bus.Event,'GetAntiAliasingState',tower))]==[samples,0,0]
    general.set_viewport_expansion_policy('FixedSize');general.set_viewport_size(1280,720)
    general.close_pane('Console')
    if timing_only or renderdoc_view:command('r_silpomMsaaInspect',0)
    for name,pose in fixture['views'].items():
        if renderdoc_view and name!=renderdoc_view:continue
        result['stage']=name;flush()
        general.set_current_view_position(*map(float,pose['position']));general.set_current_view_rotation(*map(float,pose['rotation']))
        general.idle_wait_frames(90)
        if renderdoc_view:
            import sys
            sys.path.insert(0,str(Path(__file__).parent))
            from TowerBenchmarkRenderDoc import capture_frame
            result['renderdoc']=capture_frame(OUTPUT,name,general)
        elif not timing_only:
            screenshot(OUTPUT/(name+'.png'))
            if name!='exterior':
                if samples==1:
                    capture_hit(OUTPUT/(name+'-hit.dds'))
                else:
                    # Exact integer records/depth from the existing 4W sample
                    # atlas; generic float MSAA resolves corrupt ownership bits.
                    command('r_silpomMsaaInspect',1);general.idle_wait_frames(12)
                    hierarchy=['Root','MainPipeline_0','DepthPrePass','SilPOMInspectFinal']
                    capture(OUTPUT/(name+'-hit.dds'),'Records',hierarchy)
                    capture(OUTPUT/(name+'-depth.dds'),'SampleDepth',hierarchy)
                    result['hit_capture_layout']='four-columns-per-pixel sample atlas'
                    command('r_silpomMsaaInspect',0);general.idle_wait_frames(12)
        if timing:
            for repeat in range(repeats):gpu_cost(f'{name}-{repeat+1}')
    if timing_only or renderdoc_view:
        renderer_clean()
        assert LEVEL.read_bytes()==source
        assert (PROJECT/'Levels/VueLiveLink/VueLiveLink.prefab').read_bytes()==original
        result.update(passed=True,stage='complete',saved_level_unchanged=True,
            vue_live_link_unchanged=True,gameplay_visible=False,
            scene_sha256=hashlib.sha256(source).hexdigest())
        return
    result['stage']='gameplay';flush()
    general.enter_game_mode();general.idle_enable(True);general.idle_wait(6.0)
    assert general.is_in_game_mode()
    runtime=general.find_game_entity('Irish Fieldstone Tower (Open Shaft)');assert runtime.IsValid()
    assert silpom.SilPomMeshRequestBus(bus.Event,'IsReady',runtime)
    runtime_state=silpom.SilPomMeshRequestBus(bus.Event,'GetAntiAliasingState',runtime)
    result['gameplay_aa_state']=[int(runtime_state.x),int(runtime_state.y),int(runtime_state.z)]
    assert result['gameplay_aa_state']==[samples,0,0]
    screenshot(OUTPUT/'gameplay.png')
    general.exit_game_mode();general.idle_enable(True);general.idle_wait(3.0);ready()
    renderer_clean()
    log=(PROJECT/'user/log/Editor.log').read_text(errors='replace')
    assert 'does not have baked texture assets' not in log
    assert 'Material::InitializeNonSerializedData is not supposed to fail' not in log
    assert LEVEL.read_bytes()==source
    assert (PROJECT/'Levels/VueLiveLink/VueLiveLink.prefab').read_bytes()==original
    result.update(passed=True,stage='complete',saved_level_unchanged=True,
        vue_live_link_unchanged=True,gameplay_visible=True,scene_sha256=hashlib.sha256(source).hexdigest())


try:
    flush();main()
except Exception:result['error']=traceback.format_exc()
finally:
    try:
        if general.is_in_game_mode():general.exit_game_mode();general.idle_wait(2.0)
        silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'Release')
        # Keep background ticking enabled until the final frame wait completes.
        # Restoring ed_keepEditorActive first can suspend an occluded Editor.
        general.idle_wait_frames(30)
        for key,value in cvars.items():general.run_console(f'{key} {value}')
        result['restored']=True
    except Exception:result['restore_error']=traceback.format_exc()
    flush()
    from PySide6 import QtCore
    QtCore.QTimer.singleShot(0,general.exit_no_prompt)

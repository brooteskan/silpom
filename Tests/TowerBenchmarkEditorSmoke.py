"""Validate the portable lit tower scene; optional GPU timestamps, no level saves."""
from pathlib import Path
helper=Path(__file__).with_name('MsaaEditorSmoke.py')
exec(compile(helper.read_text().split('\ndef main():')[0],str(helper),'exec'))
LEVEL=PROJECT/'Levels/SilPOMTowerBenchmark/SilPOMTowerBenchmark.prefab'


def paint_baking_off(eid):
    world=str(general.get_cvar('r_silpomPaintBake')).lower()
    receiver=json.loads(silpom.SilPomMeshRequestBus(bus.Event,'GetReceiverUvPaintStatus',eid))
    assert world in ('false','0','0.0'),world
    assert receiver['state']=='unbound',receiver
    return {'world_cache':world,'receiver_uv':receiver}


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


def capture_route_view(name):
    """Exact real-sample export at 1x/2x/4x, diagnostic runs only."""
    screenshot(OUTPUT/(name+'.png'))
    result['image_views'].append(name)
    command('r_silpomMsaaInspect',1);general.idle_wait_frames(12)
    hierarchy=['Root','MainPipeline_0','DepthPrePass','SilPOMInspectFinal']
    capture(OUTPUT/(name+'-hit.dds'),'Records',hierarchy)
    capture(OUTPUT/(name+'-depth.dds'),'SampleDepth',hierarchy)
    result['record_capture_views'].append(name)
    result['hit_capture_layout']='four-columns-per-pixel sample atlas'
    command('r_silpomMsaaInspect',0);general.idle_wait_frames(12)


def main():
    global tower,component
    timing_only=os.environ.get('SILPOM_TOWER_TIMING_ONLY','0')=='1'
    timing=os.environ.get('SILPOM_MSAA_TIMING','0')=='1'
    renderdoc_view=os.environ.get('SILPOM_TOWER_RENDERDOC_VIEW','')
    path_file=os.environ.get('SILPOM_TOWER_CAMERA_PATH','')
    camera_path=json.loads(Path(path_file).read_text()) if path_file else None
    assert not camera_path or (not timing and not renderdoc_view),'Movement validation is separate from timing/replay'
    if camera_path:
        result.update(camera_sequence=camera_path,image_views=[],record_capture_views=[],observed_camera_sequence=[])
    assert not renderdoc_view or not timing,'RenderDoc inspection must be separate from timing'
    repeats=int(os.environ.get('SILPOM_TOWER_TIMING_REPEATS','3'))
    assert not timing_only or timing,'Timing-only mode requires SILPOM_MSAA_TIMING=1'
    assert not timing or repeats>=3,'Use at least three captures per camera'
    result.update(timing_only=timing_only,timing_repeats=repeats if timing else 0)
    source=LEVEL.read_bytes()
    fixture=json.loads(LEVEL.with_name('scene.json').read_text())
    assert not renderdoc_view or renderdoc_view in fixture['views'],renderdoc_view
    assert hashlib.sha256(source).hexdigest()==fixture['scene_sha256']
    general.idle_enable(True)
    for name,value in (('ed_keepEditorActive',1),('ed_backgroundUpdatePeriod',-1),('r_displayInfo',0),
                       ('vsync_interval',0),('sys_MaxFPS',-1),('r_silpomPaintBake',0)):
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
    samples=int(os.environ.get('SILPOM_TOWER_SAMPLES',str(int(state.x))))
    assert samples in ((1,2,4) if camera_path else (1,2)),samples
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
    result['paint_baking_off']=paint_baking_off(tower)
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
            if camera_path:
                capture_route_view(name)
            else:
                screenshot(OUTPUT/(name+'.png'))
            if not camera_path and name!='exterior':
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
    if camera_path:
        for frame in camera_path['frames']:
            result['stage']=frame['name'];flush()
            general.set_current_view_position(*map(float,frame['position']))
            general.set_current_view_rotation(*map(float,frame['rotation']))
            general.idle_wait_frames(int(frame.get('settle_frames',2)))
            actual=general.get_current_view_position()
            observed=[float(actual.x),float(actual.y),float(actual.z)]
            assert max(abs(a-b) for a,b in zip(observed,frame['position']))<1e-4,(frame['name'],observed)
            result['observed_camera_sequence'].append({'name':frame['name'],'position':observed})
            if frame['capture']:capture_route_view(frame['name'])
            renderer_clean()
        result['camera_movement_complete']=True
    if timing_only or renderdoc_view:
        renderer_clean()
        result['final_paint_baking_off']=paint_baking_off(tower)
        assert LEVEL.read_bytes()==source
        result.update(passed=True,stage='complete',saved_level_unchanged=True,
            gameplay_visible=False,
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
    result['gameplay_paint_baking_off']=paint_baking_off(runtime)
    screenshot(OUTPUT/'gameplay.png')
    if camera_path:result['image_views'].append('gameplay')
    notice_timer=camera_notice_timer()
    general.exit_game_mode();general.idle_enable(True);general.idle_wait(3.0);ready()
    notice_timer.stop()
    renderer_clean()
    log=(PROJECT/'user/log/Editor.log').read_text(errors='replace')
    assert 'does not have baked texture assets' not in log
    assert 'Material::InitializeNonSerializedData is not supposed to fail' not in log
    assert LEVEL.read_bytes()==source
    result.update(passed=True,stage='complete',saved_level_unchanged=True,
        gameplay_visible=True,scene_sha256=hashlib.sha256(source).hexdigest())


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

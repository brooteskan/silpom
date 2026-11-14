"""Fresh-process gameplay matrix. No neural/lighting/quality edits or level saves.

TG_SILPOM_GAMEPLAY_CASE: baseline1, startup4, roundtrip, handoff4, history, minimal.
TG_SILPOM_ESTIMATOR_OUTPUT: per-process report/capture directory.
The minimal case generates a separate fixture from the saved raven actor assets.
"""
from pathlib import Path
helper=Path(__file__).with_name('AntiAliasingSelectorEditorSmoke.py')
exec(compile(helper.read_text().split('\ndef main():')[0],str(helper),'exec'))


def animated_fixture():
    """Create a separate lit actor-only level; never modify the tower prefab."""
    import copy
    templates=PROJECT/'user/engine-source/Assets/Editor/Prefabs'
    source=json.loads(LEVEL.read_bytes())
    default=json.loads((templates/'Default_Level.prefab').read_text())
    fixture=json.loads((templates/'Empty.prefab').read_text())
    root=fixture['ContainerEntity'];root['Name']='SilPomMsaaAnimatedRepro'
    entities={}
    for scene,name in ((source,'Raven Visual'),(default,'Sun'),(default,'Camera'),(default,'Global Sky')):
        key,value=next((k,v) for k,v in scene['Entities'].items() if v['Name']==name)
        e=copy.deepcopy(value);entities[key]=e
        for c in e['Components'].values():
            if 'TransformComponent' in c['$type']:c['Parent Entity']=root['Id']
        if name=='Raven Visual':e['Name']='AA Repro Animated Raven'
        if name=='Camera':
            for c in e['Components'].values():
                if 'TransformComponent' in c['$type']:
                    c['Transform Data']={'Translate':[0,-5,2],'Rotate':[-10,0,0],'Scale':1}
    for c in root['Components'].values():
        if c['$type']=='EditorEntitySortComponent':c['Child Entity Order']=list(entities)
    fixture.update(Entities=entities,Instances={})
    assert not any('Terrain' in c['$type'] or 'SilPOM' in c['$type']
        for e in entities.values() for c in e['Components'].values())
    path=PROJECT/'Levels/SilPomMsaaAnimatedRepro/SilPomMsaaAnimatedRepro.prefab'
    text=json.dumps(fixture,indent=4)+'\n'
    assert not path.exists() or path.read_text()==text,'Existing fixture differs; do not overwrite it'
    path.parent.mkdir(exist_ok=True);path.write_text(text)
    return path


def main():
    global tower,component
    case=os.environ.get('TG_SILPOM_GAMEPLAY_CASE','baseline1')
    assert case in ('baseline1','startup4','roundtrip','handoff4','history','minimal')
    original=LEVEL.read_bytes()
    result.update(case=case,level_sha256=hashlib.sha256(original).hexdigest(),stage='initialize')
    flush();general.idle_enable(True)
    for name,value in (('r_silpomAaTrace',int(os.environ.get('TG_SILPOM_AA_TRACE','0'))),('ed_keepEditorActive',1),('ed_backgroundUpdatePeriod',-1),
                       ('r_displayInfo',0),('vsync_interval',0),('sys_MaxFPS',-1)):
        command(name,value)
    if case=='startup4':
        # The launch command sets the bootstrap sample count before scene creation.
        command('r_silpomMsaa4',1)
    selected=animated_fixture() if case=='minimal' else LEVEL
    assert selected.exists(),selected
    assert general.open_level(str(selected));general.idle_wait_frames(90)
    if case!='minimal':
        general.set_viewport_expansion_policy('FixedSize');general.set_viewport_size(1280,720)
    if case!='minimal':
        q=entity.SearchFilter();q.names=['Irish Fieldstone Tower (Open Shaft)']
        found=entity.SearchBus(bus.Broadcast,'SearchEntities',q);assert len(found)==1;tower=found[0];ready()
        t=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['SilPOM Mesh'],entity.EntityType().Game)[0]
        component=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentOfType',tower,t).GetValue()
        props=editor.EditorComponentAPIBus(bus.Broadcast,'BuildComponentPropertyList',component)
        aa=next(p for p in props if p.endswith('|Anti-aliasing'))
        assert read(component,aa)==0,'Matrix expects the unchanged default in the saved tower'
        initial_generation=str(silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',tower))
        name,eye,direction,pose=next(camera_cases(json.loads(original)));view(eye,direction,pose)
        if case in ('roundtrip','handoff4'):
            write(component,aa,3);general.idle_wait_frames(120)
            assert aa_state()==[4,0,0],aa_state()
            if case=='roundtrip':write(component,aa,0);general.idle_wait_frames(120)
        if case=='history':
            for mode in (1,2,3,1,3,2,0):
                write(component,aa,mode);general.idle_wait_frames(90)
        assert str(silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',tower))==initial_generation
        result['editor_state']=aa_state()
    else:
        t=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['SilPOM Mesh'],entity.EntityType().Game)[0]
        q=entity.SearchFilter();q.components={t:{}}
        assert not entity.SearchBus(bus.Broadcast,'SearchEntities',q),'Minimal scene contains SilPOM'
        general.set_current_view_position(0,-5,2);general.set_current_view_rotation(-10,0,0)
        result.update(stage='native 1x to 4x',fixture=str(selected));flush()
        general.run_console('r_multiSampleCount 4');general.idle_wait_frames(120)
        result['stage']='native 4x to 1x';flush()
        general.run_console('r_multiSampleCount 1');general.idle_wait_frames(120)
    expected=4 if case in ('startup4','handoff4') else 1
    assert int(general.get_cvar('r_multiSampleCount'))==expected
    renderer_clean();result['stage']='enter gameplay';flush()
    general.enter_game_mode();general.idle_enable(True);general.idle_wait(10.0)
    assert general.is_in_game_mode()
    if case!='minimal':
        runtime=general.find_game_entity('Irish Fieldstone Tower (Open Shaft)')
        assert runtime.IsValid();result['runtime_state']=aa_state(runtime)
        result['runtime_status']=str(silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',runtime))
        assert result['runtime_state']==[expected,0,0],result['runtime_state']
        assert silpom.SilPomMeshRequestBus(bus.Event,'IsReady',runtime)
    else:
        runtime=general.find_game_entity('AA Repro Animated Raven');assert runtime.IsValid()
        result['animated_actor']=str(runtime)
    renderer_clean();result['stage']='gameplay stable';flush()
    screenshot(OUTPUT/'game.png')
    general.exit_game_mode();general.idle_enable(True);general.idle_wait(4.0)
    for _ in range(int(os.environ.get('TG_SILPOM_GAMEPLAY_REPEATS','1'))-1):
        general.enter_game_mode();general.idle_enable(True);general.idle_wait(6.0)
        assert general.is_in_game_mode();renderer_clean()
        if case!='minimal':
            runtime=general.find_game_entity('Irish Fieldstone Tower (Open Shaft)')
            assert aa_state(runtime)==[expected,0,0]
            assert silpom.SilPomMeshRequestBus(bus.Event,'IsReady',runtime)
        general.exit_game_mode();general.idle_enable(True);general.idle_wait(3.0)
        renderer_clean()
    if case=='handoff4':
        write(component,aa,0);general.idle_wait_frames(90)
        result['restored_default']=aa_state()
        assert result['restored_default']==[1,0,0],'Handoff lost the original AA baseline'
    renderer_clean();assert LEVEL.read_bytes()==original
    result.update(passed=True,stage='complete',saved_level_unchanged=True,
        gameplay_entries=int(os.environ.get('TG_SILPOM_GAMEPLAY_REPEATS','1')))


try:
    flush();main()
except Exception:result['error']=traceback.format_exc()
finally:
    flush()
    from PySide6 import QtCore
    QtCore.QTimer.singleShot(0,general.exit_no_prompt)

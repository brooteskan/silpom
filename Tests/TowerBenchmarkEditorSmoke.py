"""Validate the portable lit tower scene; optional GPU timestamps, no level saves."""
from pathlib import Path
helper=Path(__file__).with_name('MsaaEditorSmoke.py')
exec(compile(helper.read_text().split('\ndef main():')[0],str(helper),'exec'))
LEVEL=PROJECT/'Levels/SilPOMTowerBenchmark/SilPOMTowerBenchmark.prefab'


def main():
    global tower,component
    source=LEVEL.read_bytes()
    original=(PROJECT/'Levels/VueLiveLink/VueLiveLink.prefab').read_bytes()
    fixture=json.loads(LEVEL.with_name('scene.json').read_text())
    assert hashlib.sha256(source).hexdigest()==fixture['scene_sha256']
    general.idle_enable(True)
    for name,value in (('ed_keepEditorActive',1),('ed_backgroundUpdatePeriod',-1),('r_displayInfo',0),
                       ('vsync_interval',0),('sys_MaxFPS',-1)):
        command(name,value)
    assert general.open_level(str(LEVEL));general.idle_wait_frames(90)
    query=entity.SearchFilter();query.names=['Irish Fieldstone Tower (Open Shaft)']
    found=entity.SearchBus(bus.Broadcast,'SearchEntities',query);assert len(found)==1;tower=found[0];ready()
    kind=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['SilPOM Mesh'],entity.EntityType().Game)[0]
    component=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentOfType',tower,kind).GetValue()
    props=editor.EditorComponentAPIBus(bus.Broadcast,'BuildComponentPropertyList',component)
    cfg=fixture['tower_configuration']
    expected={'Anti-aliasing':0,'Lighting estimator':5,
        'Level 2 predictor (experimental)':cfg['UseLevelTwoPredictor'],
        'Gather validation (may be cheaper)':cfg['UseValidationGather'],
        'R16 height textures (less GPU memory)':cfg['UseR16HeightTextures']}
    result['controls']={}
    for label,value in expected.items():
        path=next(p for p in props if p.endswith('|'+label))
        actual=read(component,path);assert actual==value,(label,actual,value)
        result['controls'][label]=actual
    result['tower_status']=str(silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',tower))
    assert [int(x) for x in (lambda v:(v.x,v.y,v.z))(silpom.SilPomMeshRequestBus(bus.Event,'GetAntiAliasingState',tower))]==[1,0,0]
    general.set_viewport_expansion_policy('FixedSize');general.set_viewport_size(1280,720)
    general.close_pane('Console')
    for name,pose in fixture['views'].items():
        result['stage']=name;flush()
        general.set_current_view_position(*map(float,pose['position']));general.set_current_view_rotation(*map(float,pose['rotation']))
        general.idle_wait_frames(90);screenshot(OUTPUT/(name+'.png'))
        if name!='exterior':capture_hit(OUTPUT/(name+'-hit.dds'))
        gpu_cost(name)
    result['stage']='gameplay';flush()
    general.enter_game_mode();general.idle_enable(True);general.idle_wait(6.0)
    assert general.is_in_game_mode()
    runtime=general.find_game_entity('Irish Fieldstone Tower (Open Shaft)');assert runtime.IsValid()
    assert silpom.SilPomMeshRequestBus(bus.Event,'IsReady',runtime)
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
        for key,value in cvars.items():general.run_console(f'{key} {value}')
        general.idle_wait_frames(30);result['restored']=True
    except Exception:result['restore_error']=traceback.format_exc()
    flush()
    from PySide6 import QtCore
    QtCore.QTimer.singleShot(0,general.exit_no_prompt)

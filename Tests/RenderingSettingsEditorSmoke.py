"""Validate renderer-wide AA, exact 2x restoration and mesh-independent ownership.

Run in a scripted Editor with SILPOM_ESTIMATOR_OUTPUT and optional
SILPOM_CAMERA_REFERENCE set. Never saves a level.
"""
from pathlib import Path
helper=Path(__file__).with_name('MsaaEditorSmoke.py')
exec(compile(helper.read_text().split('\ndef main():')[0],str(helper),'exec'))
import silpom_rendering_controls as rendering_controls


def state():
    v=silpom.SilPomMeshRequestBus(bus.Event,'GetAntiAliasingState',tower)
    return [int(v.x),int(v.y),int(v.z)]


def select(mode,expected):
    controls.aa.setCurrentIndex(controls.aa.findData(mode))
    general.idle_wait_frames(120)
    assert rendering_controls.aa_state()['mode']==mode
    assert state()==expected,(mode,state(),expected)
    assert str(silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',tower))==generation,'AA regenerated the tower'
    assert all(read(component,p)==value for p,value in saved.items()),'AA changed material or lighting settings'
    result.setdefault('selections',[]).append({'mode':mode,'state':state()});flush()


def main():
    global tower,component,controls,generation,saved
    source=LEVEL.read_bytes()
    general.idle_enable(True)
    general.run_console('ed_keepEditorActive 1');general.run_console('ed_backgroundUpdatePeriod -1')
    general.run_console('r_displayInfo 0')
    assert general.open_level(str(LEVEL));general.idle_wait_frames(120)
    q=entity.SearchFilter();q.names=['Irish Fieldstone Tower (Open Shaft)']
    found=entity.SearchBus(bus.Broadcast,'SearchEntities',q);assert len(found)==1;tower=found[0];ready()
    kind=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['SilPOM Mesh'],entity.EntityType().Game)[0]
    component=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentOfType',tower,kind).GetValue()
    props=editor.EditorComponentAPIBus(bus.Broadcast,'BuildComponentPropertyList',component)
    assert not any(p.endswith('|Anti-aliasing') for p in props),'Renderer control still appears on the tower'
    normal=next(p for p in props if p.endswith('|Height-derived normals (off = cheaper)'))
    write(component,normal,True);ready()
    saved={p:read(component,p) for p in props if any(p.endswith('|'+name) for name in (
        'Height-derived normals (off = cheaper)','Lighting estimator','Level 2 predictor (experimental)',
        'Validation iterations (lower = cheaper)','Validation retry cap (lower = cheaper)',
        'Spiral light master brightness','Level 5A learned relief LOD (experimental)'))}
    camera_file=os.environ.get('SILPOM_CAMERA_REFERENCE')
    if camera_file:
        camera=json.loads(Path(camera_file).read_text())
        general.set_viewport_expansion_policy('FixedSize');general.set_viewport_size(*camera['viewport_size'])
        general.set_current_view_position(*camera['position']);general.set_current_view_rotation(*camera['rotation'])
    general.idle_wait_frames(90)
    controls=rendering_controls.RenderingControls()
    assert [controls.aa.itemData(i) for i in range(controls.aa.count())]==[0,1,2,4,3]
    assert state()==[2,0,0],state()
    result['baseline']=state()
    generation=str(silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',tower))
    screenshot(OUTPUT/'default-2x.png')
    for mode,expected,name in ((1,[1,1,1],'smaa'),(2,[1,2,0],'taa'),(4,[2,0,0],'msaa2'),
                               (3,[4,0,0],'msaa4'),(0,[2,0,0],'default-restored')):
        result['stage']=name;flush();select(mode,expected);screenshot(OUTPUT/(name+'.png'))
    a=QtGui.QImage(str(OUTPUT/'default-2x.png')).convertToFormat(QtGui.QImage.Format.Format_RGBA8888)
    b=QtGui.QImage(str(OUTPUT/'default-restored.png')).convertToFormat(QtGui.QImage.Format.Format_RGBA8888)
    assert bytes(a.constBits())==bytes(b.constBits()),'Returning to project default changed the rendered image'
    # No mesh owns the control. It remains functional while the tower is disabled.
    assert editor.EditorComponentAPIBus(bus.Broadcast,'DisableComponents',[component])
    rendering_controls.set_aa(2);general.idle_wait_frames(90)
    assert int(general.get_cvar('r_multiSampleCount'))==1
    rendering_controls.set_aa(4);general.idle_wait_frames(90)
    assert int(general.get_cvar('r_multiSampleCount'))==2
    assert editor.EditorComponentAPIBus(bus.Broadcast,'EnableComponents',[component]);ready()
    general.idle_wait_frames(90);assert state()==[2,0,0]
    result['mesh_independent']=True
    # The setting survives editor/runtime scene transfer at both MSAA counts.
    for mode,samples in ((4,2),(3,4)):
        rendering_controls.set_aa(mode);general.idle_wait_frames(90)
        general.enter_game_mode();general.idle_enable(True);general.idle_wait(6.0)
        assert general.is_in_game_mode()
        runtime=general.find_game_entity('Irish Fieldstone Tower (Open Shaft)');assert runtime.IsValid()
        assert silpom.SilPomMeshRequestBus(bus.Event,'IsReady',runtime)
        assert int(general.get_cvar('r_multiSampleCount'))==samples
        general.exit_game_mode();general.idle_enable(True);general.idle_wait(3.0);ready()
        assert state()==[samples,0,0]
    rendering_controls.set_aa(0);general.idle_wait_frames(90);assert state()==[2,0,0]
    assert LEVEL.read_bytes()==source
    result.update(passed=True,stage='complete',saved_level_unchanged=True,gameplay_samples=[2,4])


controls=None;saved={}
try:
    flush();main()
except Exception:result.update(passed=False,error=traceback.format_exc())
finally:
    try:
        if general.is_in_game_mode():general.exit_game_mode();general.idle_wait(3.0)
        rendering_controls.set_aa(0)
        if controls is not None:controls.deleteLater()
        general.idle_wait_frames(30)
    except Exception:result['restore_error']=traceback.format_exc()
    flush()
    from PySide6 import QtCore
    QtCore.QTimer.singleShot(0,general.exit_no_prompt)

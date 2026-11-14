"""Exercise the real editor AA property, lifecycle and approved 1x reference.

Run in an isolated Editor with TG_SILPOM_ESTIMATOR_OUTPUT set. Never saves.
"""
from pathlib import Path
helper=Path(__file__).with_name('MsaaEditorSmoke.py')
exec(compile(helper.read_text().split('\ndef main():')[0],str(helper),'exec'))

AA='Anti-aliasing'


def aa_state(target=None):
    v=silpom.SilPomMeshRequestBus(bus.Event,'GetAntiAliasingState',target or tower)
    return [int(v.x),int(v.y),int(v.z)]


def select(mode,expected,frames=90):
    write(component,properties[AA],mode)
    general.idle_wait_frames(frames)
    assert read(component,properties[AA])==mode
    assert aa_state()==expected,(mode,aa_state(),expected)
    assert silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',tower)==generation,'AA rebuilt the tower'
    assert all(read(component,p)==v for p,v in saved.items()),'AA changed tower quality/lighting'
    renderer_clean()


def capture_reference(ref,cases):
    result['stage']='approved reference';flush()
    write(component,properties[AO],False);write(component,properties[L],0)
    for p,v in ref['tower_quality_controls'].items():write(component,p,v)
    ready();general.idle_wait_frames(30)
    result['quality_controls']={p:read(component,p) for p in ref['tower_quality_controls']}
    folder=OUTPUT/'current';folder.mkdir(exist_ok=True)
    record={'views':{}};result['variants']['current']=record
    for name,eye,direction,pose in cases:
        record['views'][name]=view(eye,direction,pose);general.idle_wait_frames(45)
        screenshot(folder/(name+'.png'));reference_image(folder/(name+'.png'),ref)
        capture_hit(folder/(name+'-hit.dds'))
        if 'glancing' in name:
            for i in range(5):
                view((eye[0]+.002*i,eye[1]+.003*i,eye[2]+.002*i),direction,pose);general.idle_wait_frames(4)
                screenshot(folder/(name+f'-motion-{i}.png'))
                if i in (1,4):capture_hit(folder/(name+f'-motion-{i}-hit.dds'))
    write(component,properties[AO],False)
    for p,v in originals.items():write(component,p,v)
    ready();renderer_clean()


def main():
    global tower,component,properties,source,saved,generation
    general.idle_enable(True);source=LEVEL.read_bytes()
    assert general.open_level(str(LEVEL));general.idle_wait_frames(90)
    position,rotation=general.get_current_view_position(),general.get_current_view_rotation()
    size=general.get_viewport_size()
    initial_view.update(position=(position.x,position.y,position.z),rotation=(rotation.x,rotation.y,rotation.z),
        policy=general.get_viewport_expansion_policy(),size=(int(size.x),int(size.y)))
    q=entity.SearchFilter();q.names=['Irish Fieldstone Tower (Open Shaft)']
    towers=entity.SearchBus(bus.Broadcast,'SearchEntities',q);assert len(towers)==1;tower=towers[0];ready()
    t=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['SilPOM Mesh'],entity.EntityType().Game)[0]
    component=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentOfType',tower,t).GetValue()
    available=editor.EditorComponentAPIBus(bus.Broadcast,'BuildComponentPropertyList',component)
    properties={label:next(p for p in available if p.endswith('|'+label)) for label in (AA,L,AO,P,D,BRIGHT,NEAR)}
    assert read(component,properties[AA])==0,'Old levels must retain Current default'
    baseline=aa_state();assert baseline==[1,0,0],baseline
    result['baseline']=baseline;result['level_sha256']=hashlib.sha256(source).hexdigest()
    ref=json.loads(Path(__file__).with_name('Reference').joinpath('NearestHitVisual.json').read_text())
    for p in set(ref['tower_quality_controls'])|set(properties.values()):originals[p]=read(component,p)
    saved={p:v for p,v in originals.items() if p!=properties[AA]}
    general.set_viewport_expansion_policy('FixedSize');general.set_viewport_size(1280,720)
    for name,value in (('ed_keepEditorActive',1),('ed_backgroundUpdatePeriod',-1),('r_displayInfo',0),('vsync_interval',0),('sys_MaxFPS',-1)):command(name,value)
    general.close_pane('Console')
    cases=list(camera_cases(json.loads(source)))
    capture_reference(ref,cases)
    generation=silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',tower)
    name,eye,direction,pose=next(c for c in cases if 'glancing' in c[0]);view(eye,direction,pose)
    general.idle_wait_frames(60);screenshot(OUTPUT/'saved-default.png')
    result['stage']='selector round trip';flush()
    for mode,expected,name in ((1,[1,1,1],'smaa'),(2,[1,2,0],'taa'),(3,[4,0,0],'msaa4'),
                               (1,[1,1,1],'smaa-after-msaa'),(0,baseline,'default-restored')):
        select(mode,expected);screenshot(OUTPUT/(name+'.png'))
        result['variants'][name]={'state':aa_state()};flush()
    a=QtGui.QImage(str(OUTPUT/'saved-default.png')).convertToFormat(QtGui.QImage.Format.Format_RGBA8888)
    b=QtGui.QImage(str(OUTPUT/'default-restored.png')).convertToFormat(QtGui.QImage.Format.Format_RGBA8888)
    assert bytes(a.constBits())==bytes(b.constBits()),'Returning to default changed saved tower pixels'
    for name in ('smaa','taa','msaa4'):
        image=QtGui.QImage(str(OUTPUT/(name+'.png'))).convertToFormat(QtGui.QImage.Format.Format_RGBA8888)
        assert bytes(image.constBits())!=bytes(a.constBits()),name+' had no visible effect'
    # Consecutive edits while roots are paused must not leave the viewport paused.
    for mode in (3,1,3,2,0):
        write(component,properties[AA],mode);general.idle_wait_frames(1)
    select(0,baseline);screenshot(OUTPUT/'rapid-default.png')
    # Releasing the sole explicit request must restore global renderer settings.
    select(3,[4,0,0])
    assert editor.EditorComponentAPIBus(bus.Broadcast,'DisableComponents',[component])
    general.idle_wait_frames(30)
    assert int(general.get_cvar('r_multiSampleCount'))==1
    assert editor.EditorComponentAPIBus(bus.Broadcast,'EnableComponents',[component])
    ready();general.idle_wait_frames(60);assert aa_state()==[4,0,0]
    generation=silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',tower)
    select(0,baseline)
    renderer_clean()
    # MSAA-to-game currently exposes a native skinning-buffer state error.
    # This visual-only suite intentionally stays in the editor; it does not
    # imply that selecting 1x again makes that game-mode handoff safe.
    result['msaa_gameplay_limitation']='Select Current default, save and restart before Ctrl-G after using MSAA.'
    assert LEVEL.read_bytes()==source
    result.update(passed=True,stage='complete',saved_level_unchanged=True)


try:
    flush();main()
except Exception:result['error']=traceback.format_exc()
finally:
    try:
        if general.is_in_game_mode():general.exit_game_mode();general.idle_wait_frames(90)
        if component is not None:
            write(component,properties[AA],0);write(component,properties[AO],False)
            for p,v in originals.items():write(component,p,v)
            ready();general.idle_wait_frames(30)
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

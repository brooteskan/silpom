"""Real Inspector/API switching and Ctrl-G regression; never saves the level.

Uses the capture helpers from the estimator smoke test in the same directory.
TG_SILPOM_ESTIMATOR_OUTPUT chooses the isolated report/capture directory.
"""
from pathlib import Path
helper=Path(__file__).with_name('LightingEstimatorEditorSmoke.py')
exec(compile(helper.read_text().split('def main():')[0],str(helper),'exec'))
A='Level 4A wall material (experimental)';B='Level 4B shared material / visibility (experimental)'
P='Level 5A learned relief LOD (experimental)';D='Level 5B bounded proxy depth (experimental)'
L='Lighting estimator';S='Local shadow method'
N='Neural directional visibility (off = cheaper)';R='Learned proxy directional response (off = cheaper)'

def get(label):
    value=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentProperty',component,properties[label]);assert value.IsSuccess(),label
    return value.GetValue()

def expect(**values):
    labels=dict(a=A,b=B,p=P,d=D,l=L,s=S,n=N,r=R)
    actual={key:get(labels[key]) for key in values}
    assert actual==values,(actual,values)
    result.setdefault('transitions',[]).append(actual);flush()

def main():
    global tower,component,properties
    source=LEVEL.read_bytes();general.idle_enable(True);assert general.open_level(str(LEVEL));general.idle_wait_frames(90)
    query=entity.SearchFilter();query.names=['Irish Fieldstone Tower (Open Shaft)']
    towers=entity.SearchBus(bus.Broadcast,'SearchEntities',query);assert len(towers)==1;tower=towers[0];ready()
    types=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['SilPOM Mesh'],entity.EntityType().Game)
    response=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentOfType',tower,types[0]);assert response.IsSuccess();component=response.GetValue()
    available=editor.EditorComponentAPIBus(bus.Broadcast,'BuildComponentPropertyList',component)
    properties={label:next(p for p in available if p.endswith('|'+label)) for label in (A,B,P,D,L,S,N,R)}
    originals.update({label:get(label) for label in properties});result['loaded_choices']=originals.copy();flush()
    # Existing invalid serialized 4A + 4B must repair on activation, not wait
    # for the user to touch a checkbox or carry an old generation into Play.
    assert not (get(A) and get(B))
    if get(L):expect(a=False,b=True,s=3)
    set_value(A,True);expect(a=True,b=False,p=False,d=False,l=0,s=3)
    set_value(B,True);expect(a=False,b=True)
    set_value(A,True);set_value(P,True);expect(a=False,p=True)
    set_value(D,True);set_value(P,False);expect(p=False,d=False)
    set_value(D,True);expect(p=True,d=True,a=False)
    set_value(A,True);set_value(L,4);expect(a=False,b=True,l=4,s=3)
    set_value(B,False);expect(b=False,l=0)
    set_value(S,0);expect(a=False,b=False,p=False,d=False,n=False,r=False,l=0,s=0)
    set_value(B,True);expect(a=False,b=True,s=3)
    # Restore the accepted settings exactly before the reference/game test.
    for label in (S,A,B,P,D,L,N,R):set_value(label,originals[label])
    assert {label:get(label) for label in properties}==originals
    name,eye,direction,pose=next(camera_cases(json.loads(source)))
    view(eye,direction,pose);general.idle_wait_frames(45);screenshot(OUTPUT/'editor.png')
    result['stage']='enter game';flush();general.enter_game_mode();general.idle_wait_frames(120)
    runtime=general.find_game_entity('Irish Fieldstone Tower (Open Shaft)')
    assert runtime.IsValid(),'No game tower entity'
    result['runtime_entity']=str(runtime)
    deadline=time.monotonic()+90
    while time.monotonic()<deadline:
        result['runtime_status']=str(silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',runtime));flush()
        if silpom.SilPomMeshRequestBus(bus.Event,'IsReady',runtime):break
        general.idle_wait_frames(1)
    assert silpom.SilPomMeshRequestBus(bus.Event,'IsReady',runtime),result['runtime_status']
    screenshot(OUTPUT/'game.png');capture_hit(OUTPUT/'game-hit.dds')
    general.exit_game_mode();general.idle_wait_frames(90);ready()
    assert LEVEL.read_bytes()==source,'Saved level changed'
    result.update(passed=True,stage='complete',saved_level_unchanged=True)

try:
    flush();main()
except Exception:result['error']=traceback.format_exc()
finally:
    try:
        if general.is_in_game_mode():general.exit_game_mode();general.idle_wait_frames(60)
        if component is not None:
            for label in (S,A,B,P,D,L,N,R):
                if label in originals:set_value(label,originals[label])
        result['restored']=True
    except Exception:result['restore_error']=traceback.format_exc();result['passed']=False
    flush()
    from PySide6 import QtCore
    QtCore.QTimer.singleShot(0,general.exit_no_prompt)

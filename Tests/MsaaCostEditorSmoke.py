"""Frozen saved-tower cost/edge comparison. No quality edits or level saves.

TG_SILPOM_ESTIMATOR_OUTPUT selects a fresh output directory. Timings exclude
inspection/readback, use 120 frames twice per view, and keep all saved settings.
"""
from pathlib import Path
helper=Path(__file__).with_name('AntiAliasingSelectorEditorSmoke.py')
exec(compile(helper.read_text().split('\ndef main():')[0],str(helper),'exec'))


def hit_samples(stem):
    command('r_silpomMsaaInspect',1);general.idle_wait_frames(8)
    for slot in ('Records','SampleDepth'):
        capture(OUTPUT/f'{stem}-{slot}.dds',slot,
            ['Root','MainPipeline_0','DepthPrePass','SilPOMInspectFinal'])
    command('r_silpomMsaaInspect',0)


def main():
    global tower,component
    requested=os.environ.get('TG_SILPOM_COST_MODE','4')
    mode4=requested=='4'
    general.idle_enable(True);source=LEVEL.read_bytes()
    if mode4:command('r_silpomMsaa4',1)
    assert general.open_level(str(LEVEL));general.idle_wait_frames(90)
    q=entity.SearchFilter();q.names=['Irish Fieldstone Tower (Open Shaft)']
    found=entity.SearchBus(bus.Broadcast,'SearchEntities',q);assert len(found)==1;tower=found[0];ready()
    t=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['SilPOM Mesh'],entity.EntityType().Game)[0]
    component=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentOfType',tower,t).GetValue()
    props=editor.EditorComponentAPIBus(bus.Broadcast,'BuildComponentPropertyList',component)
    aa=next(p for p in props if p.endswith('|Anti-aliasing'))
    assert read(component,aa)==0
    ref=json.loads(Path(__file__).with_name('Reference').joinpath('NearestHitVisual.json').read_text())
    controls=set(ref['tower_quality_controls'])|{aa}
    controls.update(next(p for p in props if p.endswith('|'+label)) for label in (L,AO,P,D,BRIGHT,NEAR))
    originals.update({p:read(component,p) for p in controls})
    pos,rot=general.get_current_view_position(),general.get_current_view_rotation();size=general.get_viewport_size()
    initial_view.update(position=(pos.x,pos.y,pos.z),rotation=(rot.x,rot.y,rot.z),
        policy=general.get_viewport_expansion_policy(),size=(int(size.x),int(size.y)))
    general.set_viewport_expansion_policy('FixedSize');general.set_viewport_size(1280,720)
    for name,value in (('ed_keepEditorActive',1),('ed_backgroundUpdatePeriod',-1),('r_displayInfo',0),
                       ('vsync_interval',0),('sys_MaxFPS',-1),('r_silpomMsaaInspect',0),('r_silpomMsaaRawCoverage',1)):
        command(name,value)
    general.close_pane('Console')
    cases=list(camera_cases(json.loads(source)))
    result.update(level_sha256=hashlib.sha256(source).hexdigest(),saved_tower_controls=originals,
        generation=str(silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',tower)))
    modes=((0,'default'),(3,'msaa4')) if requested=='switch' else ((0,'msaa4' if mode4 else 'default'),)
    for mode,label in modes:
        mode4=label=='msaa4'
        write(component,aa,mode);general.idle_wait_frames(180)
        assert aa_state()==([4,0,0] if mode4 else [1,0,0])
        for name,eye,direction,pose in cases:
            result['stage']=label+'/'+name;flush();view(eye,direction,pose);general.idle_wait_frames(180)
            for repeat in range(2):gpu_cost(f'{label}-{name}-{repeat}')
            if os.environ.get('TG_SILPOM_COST_TIMING_ONLY','0')=='1':continue
            screenshot(OUTPUT/f'{label}-{name}.png')
            if mode4:
                hit_samples(label+'-'+name)
                command('r_silpomMsaaRawCoverage',0);general.idle_wait_frames(12)
                screenshot(OUTPUT/f'repaired-{name}.png');hit_samples('repaired-'+name)
                command('r_silpomMsaaRawCoverage',1);general.idle_wait_frames(12)
            else:capture_hit(OUTPUT/f'{label}-{name}-Records.dds')
            for i in range(8):
                view((eye[0]+.001*i,eye[1]+.0015*i,eye[2]+.001*i),direction,pose)
                general.idle_wait_frames(4);screenshot(OUTPUT/f'{label}-{name}-motion-{i}.png')
                if mode4 and 'glancing' in name and i in (1,4):hit_samples(f'{label}-{name}-motion-{i}')
        assert all(read(component,p)==v for p,v in originals.items() if p!=aa),'Quality/lighting changed'
        assert str(silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',tower))==result['generation'],'AA rebuilt tower'
        renderer_clean()
    assert LEVEL.read_bytes()==source
    result.update(passed=True,stage='complete',saved_level_unchanged=True)


try:
    flush();main()
except Exception:result['error']=traceback.format_exc()
finally:
    try:
        silpom.SilPomPlanarBenchmarkBus(bus.Broadcast,'Release')
        if component is not None and originals:
            aa=next(p for p in originals if p.endswith('|Anti-aliasing'));write(component,aa,originals[aa])
        general.run_console('r_silpomMsaaInspect 0');general.idle_wait_frames(45)
        for key,value in cvars.items():
            # Startup MSAA is process-local. Keep it stable through shutdown;
            # the level's AA property remains unchanged throughout this test.
            if key!='r_silpomMsaa4':general.run_console(f'{key} {value}')
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

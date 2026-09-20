"""Isolate saved scene shadow casters in an unsaved Editor session."""
import json, os, traceback
from pathlib import Path
import azlmbr.atom as atom
import azlmbr.bus as bus
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.paths as paths
import azlmbr.render as render

output=Path(paths.projectroot)/'user'/os.environ.get('SILPOM_FACE_OUTPUT','SilPOMShadowIsolation')
output.mkdir(parents=True,exist_ok=True)
result={'passed':False,'captures':[],'entities':{}}
def flush(): (output/'result.json').write_text(json.dumps(result,indent=2))
def capture(name):
    result['stage']=name;flush()
    # Level loading restores its saved camera asynchronously. Set the comparison
    # view after that restoration, and again for every isolation capture.
    general.set_current_view_position(18.5,13.5,40.2);general.set_current_view_rotation(0.0,0.0,45.0)
    general.update_viewport();general.idle_wait_frames(30)
    filename=str(output/(name+'.png'))
    request=atom.FrameCaptureRequestBus(bus.Broadcast,'CaptureScreenshot',filename)
    assert request.IsSuccess()
    done={};handler=atom.FrameCaptureNotificationBusHandler();handler.connect(request.GetValue())
    handler.add_callback('OnFrameCaptureFinished',lambda args:done.update(success=args[0]==atom.FrameCaptureResult_Success))
    for _ in range(300):
        if done:break
        general.idle_wait_frames(1)
    handler.disconnect();assert done.get('success'),filename
    result['captures'].append(filename);flush()
try:
    general.idle_enable(True);assert general.open_level('DefaultLevel')
    for command in ('ed_backgroundUpdatePeriod -1','r_meshInstancingEnabled false','r_multiSampleCount 1','r_displayInfo 0'):
        general.run_console(command)
    render.DirectionalLightRequestBus(bus.Broadcast,'SetShadowmapSize',512)
    render.DirectionalLightRequestBus(bus.Broadcast,'SetCascadeCount',1)
    render.DirectionalLightRequestBus(bus.Broadcast,'SetShadowFarClipDistance',20.0)
    general.set_viewport_expansion_policy('FixedSize');general.set_viewport_size(1280,720);general.update_viewport()
    general.set_current_view_position(18.5,13.5,40.2);general.set_current_view_rotation(0.0,0.0,45.0)
    general.idle_wait_frames(90);capture('baseline')
    for name in ('SilPOM_Planar_FBX_Edge_Study','Grid_001','silpom test','SilPOM_Planar_AB_UNSAVED'):
        search=entity.SearchFilter();search.names=[name]
        found=entity.SearchBus(bus.Broadcast,'SearchEntities',search)
        result['entities'][name]=len(found)
        for other in found:editor.EditorEntityAPIBus(bus.Event,'SetVisibilityState',other,False)
        capture('without_'+name.replace(' ','_'))
        for other in found:editor.EditorEntityAPIBus(bus.Event,'SetVisibilityState',other,True)
    old_intensity=render.DirectionalLightRequestBus(bus.Broadcast,'GetIntensity')
    render.DirectionalLightRequestBus(bus.Broadcast,'SetIntensity',0.0);capture('without_directional_light')
    render.DirectionalLightRequestBus(bus.Broadcast,'SetIntensity',old_intensity)
    capture('restored');result['passed']=True
except Exception:result['error']=traceback.format_exc()
finally:flush();print(json.dumps(result,indent=2))

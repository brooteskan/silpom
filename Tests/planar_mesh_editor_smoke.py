"""Launch a separate Editor with --runpython; leaves the unsaved fixture visible.

Set SILPOM_FACE_EXIT=1 to exit after captures. Never saves DefaultLevel.
"""
import json, math, os, traceback
from pathlib import Path
import azlmbr.asset as asset
import azlmbr.atom as atom
import azlmbr.bus as bus
import azlmbr.components as components
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.math as azmath
import azlmbr.paths as paths
import azlmbr.render as render
import azlmbr.silpom as silpom

output=Path(paths.projectroot)/'user'/os.environ.get('SILPOM_FACE_OUTPUT','SilPOMPlanarFaces')
output.mkdir(parents=True,exist_ok=True)
result={'passed':False,'captures':[],
        'scope':'Component lifecycle and screenshot capture; images require separate visual review'}
def flush(): (output/'result.json').write_text(json.dumps(result,indent=2))
def asset_id(name):
    value=asset.AssetCatalogRequestBus(bus.Broadcast,'GetAssetIdByPath',name,azmath.Uuid(),False)
    assert value.is_valid(),name
    return value
def set_property(path,value):
    outcome=editor.EditorComponentAPIBus(bus.Broadcast,'SetComponentProperty',component,path,value)
    assert outcome.IsSuccess(),str(outcome.GetError())
def wait_ready():
    for _ in range(600):
        if silpom.SilPomMeshRequestBus(bus.Event,'IsReady',mesh):return
        general.idle_wait_frames(1)
    raise RuntimeError(silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',mesh))
def capture(name):
    general.idle_wait_frames(30)
    filename=str(output/(name+'.png'))
    outcome=atom.FrameCaptureRequestBus(bus.Broadcast,'CaptureScreenshot',filename)
    assert outcome.IsSuccess()
    done={};handler=atom.FrameCaptureNotificationBusHandler();handler.connect(outcome.GetValue())
    handler.add_callback('OnFrameCaptureFinished',lambda args:done.update(success=args[0]==atom.FrameCaptureResult_Success))
    for _ in range(300):
        if done:break
        general.idle_wait_frames(1)
    handler.disconnect();assert done.get('success'),filename
    result['captures'].append(filename);flush()

try:
    flush();general.idle_enable(True)
    assert general.open_level('DefaultLevel')
    general.idle_wait_frames(30)
    # The artist may have saved an earlier test fixture in the level. Replace
    # only that named fixture in this UNSAVED session, so tests never overlap it.
    fixture_filter=entity.SearchFilter();fixture_filter.names=['SilPOM_Planar_FBX_Edge_Study']
    for previous in entity.SearchBus(bus.Broadcast,'SearchEntities',fixture_filter):
        editor.ToolsApplicationRequestBus(bus.Broadcast,'DeleteEntityById',previous)
    general.idle_wait_frames(5)
    general.run_console('ed_backgroundUpdatePeriod -1')
    general.run_console('r_meshInstancingEnabled false')
    general.run_console('r_multiSampleCount 1')
    general.run_console('r_displayInfo 0')
    render.DirectionalLightRequestBus(bus.Broadcast,'SetShadowmapSize',512)
    render.DirectionalLightRequestBus(bus.Broadcast,'SetCascadeCount',1)
    render.DirectionalLightRequestBus(bus.Broadcast,'SetShadowFarClipDistance',20.0)
    general.set_viewport_expansion_policy('FixedSize');general.set_viewport_size(1280,720);general.update_viewport()
    mesh=editor.ToolsApplicationRequestBus(bus.Broadcast,'CreateNewEntity',entity.EntityId())
    editor.EditorEntityAPIBus(bus.Event,'SetName',mesh,'SilPOM_Planar_FBX_Edge_Study')
    types=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['SilPOM Mesh'],entity.EntityType().Game)
    added=editor.EditorComponentAPIBus(bus.Broadcast,'AddComponentsOfType',mesh,types)
    assert added.IsSuccess(),str(added.GetError())
    component=added.GetValue()[0]
    set_property('Controller|Configuration|Mesh surface',asset_id('assets/silpomplanarfaces/planar_faces.fbx.silpommesh'))
    components.TransformBus(bus.Event,'SetWorldTranslation',mesh,azmath.Vector3(16.0,16.0,40.0))
    general.set_current_view_position(16.0,12.5,40.0);general.set_current_view_rotation(0.0,0.0,0.0)
    for _ in range(300):
        status=silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',mesh)
        if 'discovered' in status or 'Assign material' in status:break
        general.idle_wait_frames(1)
    properties=editor.EditorComponentAPIBus(bus.Broadcast,'BuildComponentPropertyList',component)
    fixture=json.loads((Path(paths.projectroot)/'Assets/SilPOMPlanarFaces/fixture.json').read_text())
    scale_path=None
    for path in properties:
        if path.endswith('|Authored material ID'):
            identity=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentProperty',component,path).GetValue()
            material=fixture['materials'][identity].replace('.material','.azmaterial')
            set_property(path.rsplit('|',1)[0]+'|Material',asset_id('assets/silpomplanarfaces/'+material))
        elif path.endswith('|Height image'):
            set_property(path,asset_id('assets/silpomplanarfaces/height.png.streamingimage'))
        elif path.endswith('|Displacement (world metres)'):
            scale_path=path;set_property(path,.3)
        elif path.endswith('|UV offset'):
            # Put nonzero relief at the fold and at the ordinary/tagged edge.
            set_property(path,azmath.Vector2(.035,.02))
    assert scale_path
    wait_ready();result['status']=silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',mesh);flush()
    editor.ToolsApplicationRequestBus(bus.Broadcast,'SetSelectedEntities',[])
    capture('front_displaced')
    set_property(scale_path,0.0);wait_ready();capture('front_flat')
    set_property(scale_path,.3);wait_ready()
    for name,position,rotation in [
        ('fold_close',(16,14.5,40.25),(0,0,0)),
        ('oblique',(18.5,13.5,40.2),(0,0,45)),
        ('grazing',(19,15.7,40.3),(0,0,84)),
        ('off_centre',(16,12.5,40),(0,0,25)),
        ('near_plane',(15.5,15.8,40.5),(0,0,0)),
    ]:
        general.set_current_view_position(*map(float,position));general.set_current_view_rotation(*map(float,rotation));capture(name)
    # Same camera and lighting for external-shadow and relief-only comparisons.
    general.set_current_view_position(18.5,13.5,40.2);general.set_current_view_rotation(0.0,0.0,45.0)
    capture('shadow_relief_on')
    shadow_path='Controller|Configuration|Relief shadow steps'
    set_property(shadow_path,0);wait_ready();capture('shadow_relief_off')
    set_property(scale_path,0.0);wait_ready();capture('shadow_base_mesh')
    set_property(scale_path,.3);set_property(shadow_path,16);wait_ready()
    result['relief_shadow_steps']=16
    for _ in range(3):
        assert editor.EditorComponentAPIBus(bus.Broadcast,'DisableComponents',[component]);general.idle_wait_frames(3)
        assert editor.EditorComponentAPIBus(bus.Broadcast,'EnableComponents',[component]);wait_ready()
    result['activation_cycles']=3
    # Invalid profile edits must retain the last visible generation.
    set_property(scale_path,float('nan'));general.idle_wait_frames(10)
    result['invalid_status']=silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',mesh)
    assert not silpom.SilPomMeshRequestBus(bus.Event,'IsReady',mesh)
    set_property(scale_path,.3);wait_ready()
    general.set_current_view_position(16.0,12.5,40.0);general.set_current_view_rotation(0.0,0.0,0.0)
    result['passed']=True
except Exception:
    result['error']=traceback.format_exc()
finally:
    flush();print(json.dumps(result,indent=2))
    if os.environ.get('SILPOM_FACE_EXIT')=='1':general.exit_no_prompt()

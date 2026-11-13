"""Level 5A render correctness only. No timing capture or benchmark service."""
import hashlib,json,math,os,struct,time,traceback
from pathlib import Path
import azlmbr.atom as atom
import azlmbr.bus as bus
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.paths as paths
import azlmbr.silpom as silpom
from PySide6 import QtCore,QtWidgets,QtGui
PROJECT=Path(paths.projectroot)
LEVEL=PROJECT/'Levels/VueLiveLink/VueLiveLink.prefab'
OUTPUT=Path(os.environ['TG_SILPOM_LEVEL_FIVE_TEST_OUTPUT']);OUTPUT.mkdir(parents=True,exist_ok=True)
DEPTH='Relief depth multiplier';METHOD='Local shadow method';MATERIAL='Level 4A wall material (experimental)'
BRIGHT='Spiral light master brightness';NORMAL='Height-derived normals (off = cheaper)'
result={'passed':False,'benchmark_performed':False,'stage':'startup'}
originals={};properties={};component=None;canvas=None;initial_view={};cvars={}
def flush():(OUTPUT/'results.json').write_text(json.dumps(result,indent=2))

def ready():
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        general.idle_wait_frames(1)
        if silpom.SilPomMeshRequestBus(bus.Event, "IsReady", tower):
            return
    raise RuntimeError(str(silpom.SilPomMeshRequestBus(bus.Event, "GetStatus", tower)))

def set_value(label, value):
    response = editor.EditorComponentAPIBus(bus.Broadcast, "SetComponentProperty", component, properties[label], value)
    assert response.IsSuccess(), (label, str(response.GetError()))
    actual = editor.EditorComponentAPIBus(bus.Broadcast, "GetComponentProperty", component, properties[label])
    assert actual.IsSuccess(), label
    observed=actual.GetValue()
    assert (math.isclose(observed,value,abs_tol=1e-6) if isinstance(value,float) else observed==value), (label,str(observed),value)
    ready()

def screenshot(path):
    done = []
    handler = atom.FrameCaptureNotificationBusHandler()
    response = atom.FrameCaptureRequestBus(bus.Broadcast, "CaptureScreenshot", str(path))
    assert response.IsSuccess(), str(response.GetError())
    handler.connect(response.GetValue())
    handler.add_callback("OnFrameCaptureFinished", lambda args: done.append(args[0]))
    try:
        deadline = time.monotonic() + 45
        while not done and time.monotonic() < deadline:
            general.idle_wait_frames(1)
        assert done == [atom.FrameCaptureResult_Success], (str(path), done)
    finally:
        handler.disconnect()

def view(eye, direction, pose):
    angle = math.radians(pose["Rotate"][2])
    c, s = math.cos(angle), math.sin(angle)
    x, y, z = pose["Translate"]
    position = (x + c * eye[0] - s * eye[1], y + s * eye[0] + c * eye[1], z + eye[2])
    yaw = math.degrees(math.atan2(direction[1], direction[0])) - 90 + pose["Rotate"][2]
    pitch = math.degrees(math.atan2(direction[2], math.hypot(*direction[:2])))
    rotation = (pitch, 0.0, (yaw + 180) % 360 - 180)
    general.set_current_view_position(*position)
    general.set_current_view_rotation(*rotation)
    general.idle_wait_frames(2)
    actual = general.get_current_view_position()
    assert max(abs(a - b) for a, b in zip(position, (actual.x, actual.y, actual.z))) < .002
    return {"position": position, "rotation": rotation}

def camera_cases(level):
    instance = next(v for v in level["Instances"].values()
                    if v["Source"].endswith("IrishFieldstoneTowerOpenShaft.prefab"))
    source = PROJECT / instance["Source"]
    prefab = json.loads(source.read_bytes())
    pose = prefab["ContainerEntity"]["Components"]["TransformComponent"].get(
        "Transform Data", {"Translate": [0, 0, 0], "Rotate": [0, 0, 0]})
    prefix = "/ContainerEntity/Components/TransformComponent/Transform Data/"
    for patch in instance["Patches"]:
        if patch["op"] in ("add", "replace") and patch["path"] == prefix[:-1]:
            pose = patch["value"]
        if patch["op"] == "replace" and patch["path"].startswith(prefix):
            parts = patch["path"][len(prefix):].split("/")
            if len(parts) == 2 and parts[0] in ("Translate", "Rotate"):
                pose[parts[0]][int(parts[1])] = patch["value"]
    mesh = json.loads(source.with_name("tower-open-shaft-no-landings-silpom.silpom.json").read_bytes())
    materials = {m["id"] for m in mesh["materials"] if "sculpt relief" in m["name"]}
    treads = []
    for face in mesh["faces"]:
        if face["material_id"] not in materials:
            continue
        a, b, c = [mesh["vertices"][str(i)] for i in face["vertices"]]
        if max(a[2], b[2], c[2]) - min(a[2], b[2], c[2]) > .07:
            continue
        point = tuple((u + v + w) / 3 for u, v, w in zip(a, b, c))
        if 1.36 < math.hypot(*point[:2]) < 1.55:
            treads.append(point)
    assert treads, "No camera fixture treads found"
    for name, height in (("lower", 8.39), ("upper", 16.59)):
        x, y, z = min(treads, key=lambda t: abs(t[2] - height))
        eye = (x, y, z + 1.58)
        azimuth = math.atan2(y, x)
        tangent = azimuth + math.pi - math.asin(.905 / math.hypot(x, y))
        yield name + "-glancing", eye, (math.cos(tangent), math.sin(tangent), .16), pose
        if name == "lower":
            yield "lower-facing", eye, (math.cos(azimuth), math.sin(azimuth), -.04), pose

def capture_attachment(path,slot):
    done=[];handler=atom.FrameCaptureNotificationBusHandler()
    response=atom.FrameCaptureRequestBus(bus.Broadcast,'CapturePassAttachment',str(path),
        ['Root','MainPipeline_0','OpaquePass','SilPOMHitForward'],slot,1)
    assert response.IsSuccess(),str(response.GetError())
    handler.connect(response.GetValue());handler.add_callback('OnFrameCaptureFinished',lambda args:done.append(args[0]))
    try:
        deadline=time.monotonic()+45
        while not done and time.monotonic()<deadline:general.idle_wait_frames(1)
        assert done==[atom.FrameCaptureResult_Success],(slot,done)
    finally:handler.disconnect()

def hit_signature(path):
    raw=path.read_bytes();assert raw[:4]==b'DDS '
    header=148 if raw[84:88]==b'DX10' else 128
    width,height=struct.unpack_from('<I',raw,16)[0],struct.unpack_from('<I',raw,12)[0]
    if header==148:assert struct.unpack_from('<I',raw,128)[0]==3,'Expected RGBA32_UINT hit records'
    pixels=raw[header:header+width*height*16];assert len(pixels)==width*height*16
    # Ownership allocations change when a material generation is rebuilt.
    # Compare UV, depth and status, excluding the allocation/face-owner number.
    signature=bytearray()
    for x,y,z,w in struct.iter_unpack('<4I',pixels):signature+=struct.pack('<4I',x,y,z&0x7fffffff,w&3)
    return hashlib.sha256(signature).hexdigest()


PROXY='Level 5A learned relief LOD (experimental)'
BUDGET='Level 5A error budget (higher = more proxies)'
sun=None

def records(path):
    raw=path.read_bytes();assert raw[:4]==b'DDS ' and raw[84:88]==b'DX10'
    assert struct.unpack_from('<I',raw,128)[0]==3
    return list(struct.iter_unpack('<4I',raw[148:]))

def proxy_state():
    v=silpom.SilPomMeshRequestBus(bus.Event,'GetProxyLodState',tower)
    assert v.z<=.7501
    return [v.x,v.y,v.z]

def verify_proxy_capture(name):
    general.idle_wait_frames(15)
    owners=set(silpom.SilPomMeshRequestBus(bus.Event,'GetProxyFaceOwners',tower));assert owners,'No selected proxy faces'
    state=proxy_state();assert state[0]>0 and state[0]<=state[1]
    path=OUTPUT/(name+'-hit.dds');capture_attachment(path,'Hit')
    hits=[p for p in records(path) if p[3]>>2 in owners]
    result['last_capture']={'name':name,'state':state,'selected_owners':sorted(owners),
        'selected_pixels':len(hits),'proxy_pixels':sum(bool(p[2]&0x80000000) for p in hits),
        'refined_pixels':sum(not bool(p[2]&0x80000000) for p in hits)};flush()
    assert hits,'No visible proxy pixels'
    assert all(p[3]&3==1 and p[2]&0x80000000 for p in hits),'A selected proxy published a refined/failed hit'
    screenshot(OUTPUT/(name+'.png'))
    result.setdefault('proxy_views',{})[name]={'state':state,'visible_proxy_pixels':len(hits)};flush()
    return state

def main():
    global tower,component,properties,canvas,sun
    source=LEVEL.read_bytes();result['level_sha256']=hashlib.sha256(source).hexdigest()
    general.idle_enable(True);assert general.open_level(str(LEVEL));general.idle_wait_frames(90)
    p,t=general.get_current_view_position(),general.get_current_view_rotation();size=general.get_viewport_size()
    initial_view.update(position=(p.x,p.y,p.z),rotation=(t.x,t.y,t.z),policy=general.get_viewport_expansion_policy(),size=(int(size.x),int(size.y)))
    query=entity.SearchFilter();query.names=['Irish Fieldstone Tower (Open Shaft)']
    towers=entity.SearchBus(bus.Broadcast,'SearchEntities',query);assert len(towers)==1;tower=towers[0];ready()
    types=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['SilPOM Mesh'],entity.EntityType().Game)
    found=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentOfType',tower,types[0]);assert found.IsSuccess();component=found.GetValue()
    available=editor.EditorComponentAPIBus(bus.Broadcast,'BuildComponentPropertyList',component)
    for label in (DEPTH,METHOD,MATERIAL,PROXY,BUDGET,BRIGHT,NORMAL):
        properties[label]=next(p for p in available if p.endswith('|'+label))
        v=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentProperty',component,properties[label]);assert v.IsSuccess();originals[label]=v.GetValue()
    result['original_settings']=originals.copy();flush()
    for label,value in ((PROXY,False),(MATERIAL,False),(DEPTH,.5),(METHOD,3),(NORMAL,True),(BUDGET,.75)):
        v=editor.EditorComponentAPIBus(bus.Broadcast,'SetComponentProperty',component,properties[label],value);assert v.IsSuccess()
    ready()
    for key,value in (('ed_keepEditorActive',1),('ed_backgroundUpdatePeriod',-1),('r_displayInfo',0),('r_antiAliasing',0)):
        cvars[key]=general.get_cvar(key);general.run_console(f'{key} {value}')
    general.close_pane('Console');general.set_viewport_expansion_policy('FixedSize');general.idle_wait_frames(10)
    general.set_viewport_size(640,400);general.update_viewport();general.idle_wait_frames(30)
    cases=list(camera_cases(json.loads(source)));name,eye,direction,pose=cases[0];view(eye,direction,pose)
    # Level 4 remains an independent selectable material path.
    capture_attachment(OUTPUT/'level4-off.dds','Hit');set_value(MATERIAL,True);general.idle_wait_frames(20)
    capture_attachment(OUTPUT/'level4-on.dds','Hit')
    assert hit_signature(OUTPUT/'level4-off.dds')==hit_signature(OUTPUT/'level4-on.dds')
    result['level4_hit_regression_passed']=True
    v=editor.EditorComponentAPIBus(bus.Broadcast,'SetComponentProperty',component,properties[PROXY],True);assert v.IsSuccess();general.idle_wait_frames(10)
    assert not silpom.SilPomMeshRequestBus(bus.Event,'IsReady',tower)
    assert 'disable Level 4A' in str(silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',tower))
    set_value(MATERIAL,False);ready()
    result['stage']='far front';flush();view((60,0,12),(-1,0,0),pose);general.idle_wait_frames(30)
    first=verify_proxy_capture('far-front')
    result['stage']='resize';flush();general.set_viewport_size(1280,800);general.update_viewport();general.idle_wait_frames(30)
    high=proxy_state();assert high[0]<=first[0],(first,high)
    result['resize']={'small':first,'large':high}
    general.set_viewport_size(640,400);general.update_viewport();general.idle_wait_frames(30)
    view((45,45,12),(-1,-1,0),pose);general.idle_wait_frames(20);verify_proxy_capture('far-quarter')
    view((120,0,12),(-1,0,0),pose);general.idle_wait_frames(20);verify_proxy_capture('far-distant')
    # Walk through the geometric threshold without changing the selection policy.
    result['transition_states']=[]
    for distance in (120,80,60,40,20,8,4):
        view((distance,0,12),(-1,0,0),pose);general.idle_wait_frames(10)
        result['transition_states'].append({'distance':distance,'state':proxy_state()})
    assert result['transition_states'][-1]['state'][0]==0,'Close wall retained distant proxies'
    screenshot(OUTPUT/'near-after-transition.png')
    view((60,0,12),(-1,0,0),pose);general.idle_wait_frames(30);verify_proxy_capture('decal-view')
    # Change an actual directional light, keeping external shadow integration.
    lightTypes=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['Directional Light'],entity.EntityType().Game)
    assert lightTypes,'No directional light component type'
    for eid in entity.SearchBus(bus.Broadcast,'SearchEntities',entity.SearchFilter()):
        response=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentOfType',eid,lightTypes[0])
        if not response.IsSuccess():continue
        candidate=response.GetValue();props=editor.EditorComponentAPIBus(bus.Broadcast,'BuildComponentPropertyList',candidate)
        path=next((p for p in props if p.endswith('|Intensity')),None)
        if not path:continue
        original=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentProperty',candidate,path)
        if original.IsSuccess() and float(original.GetValue())>0:sun=(candidate,path,original.GetValue());break
    assert sun,'No positive directional light found'
    screenshot(OUTPUT/'sun-on.png')
    response=editor.EditorComponentAPIBus(bus.Broadcast,'SetComponentProperty',sun[0],sun[1],0.0);assert response.IsSuccess()
    general.idle_wait_frames(20);screenshot(OUTPUT/'sun-off.png')
    assert (OUTPUT/'sun-on.png').read_bytes()!=(OUTPUT/'sun-off.png').read_bytes()
    response=editor.EditorComponentAPIBus(bus.Broadcast,'SetComponentProperty',sun[0],sun[1],sun[2]);assert response.IsSuccess();general.idle_wait_frames(20)
    result['dynamic_light']=True
    canvas=silpom.DecalPainterBus(bus.Broadcast,'CreatePaintEntity')
    panel=next(w for w in QtWidgets.QApplication.allWidgets() if w.objectName()=='DecalPainter')
    viewport=QtWidgets.QWidget.find(silpom.DecalPainterBus(bus.Broadcast,'GetViewportWindowId'))
    panel.findChild(QtWidgets.QComboBox,'PaintMode').setCurrentIndex(0)
    panel.findChild(QtWidgets.QDoubleSpinBox,'BrushDiameter').setValue(2.0)
    panel.findChild(QtWidgets.QDoubleSpinBox,'BrushOpacity').setValue(1.0)
    panel.findChild(QtWidgets.QCheckBox,'EnablePainting').setChecked(True)
    editor.ToolsApplicationRequestBus(bus.Broadcast,'SetSelectedEntities',[]);general.idle_wait_frames(20)
    screenshot(OUTPUT/'before-decal.png');pick=silpom.DecalPainterBus(bus.Broadcast,'PickVisiblePixel',320,200);assert pick.GetLength()>1
    local=QtCore.QPointF(320/viewport.devicePixelRatioF(),200/viewport.devicePixelRatioF())
    for kind,buttons in ((QtCore.QEvent.Type.MouseButtonPress,QtCore.Qt.MouseButton.LeftButton),(QtCore.QEvent.Type.MouseButtonRelease,QtCore.Qt.MouseButton.NoButton)):
        event=QtGui.QMouseEvent(kind,local,viewport.mapToGlobal(local),QtCore.Qt.MouseButton.LeftButton,buttons,QtCore.Qt.KeyboardModifier.NoModifier)
        QtWidgets.QApplication.sendEvent(viewport,event)
    general.idle_wait_frames(20);assert silpom.DecalPaintRequestBus(bus.Event,'GetDabCount',canvas)>0
    screenshot(OUTPUT/'after-decal.png');assert (OUTPUT/'before-decal.png').read_bytes()!=(OUTPUT/'after-decal.png').read_bytes()
    panel.findChild(QtWidgets.QCheckBox,'EnablePainting').setChecked(False);general.undo();general.idle_wait_frames(10)
    assert silpom.DecalPaintRequestBus(bus.Event,'GetDabCount',canvas)==0
    result['decal_and_undo']=True;assert LEVEL.read_bytes()==source;result.update(passed=True,stage='complete')
try:
    flush();main()
except Exception:result['error']=traceback.format_exc()
finally:
    try:
        if sun is not None:
            response=editor.EditorComponentAPIBus(bus.Broadcast,'SetComponentProperty',sun[0],sun[1],sun[2]);assert response.IsSuccess()
        if canvas is not None:editor.ToolsApplicationRequestBus(bus.Broadcast,'DeleteEntityById',canvas)
        if component is not None:
            for label,value in originals.items():
                response=editor.EditorComponentAPIBus(bus.Broadcast,'SetComponentProperty',component,properties[label],value);assert response.IsSuccess()
            ready()
        for key,value in cvars.items():general.run_console(f'{key} {value}')
        if initial_view:
            general.set_current_view_position(*initial_view['position']);general.set_current_view_rotation(*initial_view['rotation'])
            general.set_viewport_expansion_policy(initial_view['policy']);general.idle_wait_frames(20)
            if initial_view['policy']=='FixedSize':general.set_viewport_size(*initial_view['size']);general.idle_wait_frames(20)
        result['restored']=True
    except Exception:result['restore_error']=traceback.format_exc();result['passed']=False
    flush();QtCore.QTimer.singleShot(0,general.exit_no_prompt)

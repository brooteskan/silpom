"""Level 4A render correctness only. No timing capture or benchmark service."""
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
OUTPUT=Path(os.environ['TG_SILPOM_LEVEL_FOUR_TEST_OUTPUT']);OUTPUT.mkdir(parents=True,exist_ok=True)
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

def main():
    global tower,component,properties,canvas
    source=LEVEL.read_bytes();result['level_sha256']=hashlib.sha256(source).hexdigest()
    general.idle_enable(True);assert general.open_level(str(LEVEL));general.idle_wait_frames(90)
    p,t=general.get_current_view_position(),general.get_current_view_rotation();size=general.get_viewport_size()
    initial_view.update(position=(p.x,p.y,p.z),rotation=(t.x,t.y,t.z),policy=general.get_viewport_expansion_policy(),size=(int(size.x),int(size.y)))
    query=entity.SearchFilter();query.names=['Irish Fieldstone Tower (Open Shaft)']
    towers=entity.SearchBus(bus.Broadcast,'SearchEntities',query);assert len(towers)==1;tower=towers[0];ready()
    types=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['SilPOM Mesh'],entity.EntityType().Game)
    found=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentOfType',tower,types[0]);assert found.IsSuccess();component=found.GetValue()
    available=editor.EditorComponentAPIBus(bus.Broadcast,'BuildComponentPropertyList',component)
    for label in (DEPTH,METHOD,MATERIAL,BRIGHT,NORMAL):
        properties[label]=next(p for p in available if p.endswith('|'+label))
        v=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentProperty',component,properties[label]);assert v.IsSuccess();originals[label]=v.GetValue()
    result['original_settings']=originals.copy()
    # Rejection before inference must retain an explicit invalid status.
    for label,value in ((MATERIAL,False),(METHOD,0)):
        response=editor.EditorComponentAPIBus(bus.Broadcast,'SetComponentProperty',component,properties[label],value);assert response.IsSuccess()
    response=editor.EditorComponentAPIBus(bus.Broadcast,'SetComponentProperty',component,properties[MATERIAL],True);assert response.IsSuccess()
    general.idle_wait_frames(10)
    status=str(silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',tower))
    assert 'requires neural-only' in status and not silpom.SilPomMeshRequestBus(bus.Event,'IsReady',tower),status
    result['unsupported_mode_rejected']=status
    for label,value in ((MATERIAL,False),(DEPTH,.5),(METHOD,3)):
        response=editor.EditorComponentAPIBus(bus.Broadcast,'SetComponentProperty',component,properties[label],value);assert response.IsSuccess()
    ready()
    for key,value in (('ed_keepEditorActive',1),('ed_backgroundUpdatePeriod',-1),('r_displayInfo',0),('r_antiAliasing',0)):
        cvars[key]=general.get_cvar(key);general.run_console(f'{key} {value}')
    general.close_pane('Console');general.set_viewport_expansion_policy('FixedSize');general.idle_wait_frames(10)
    general.set_viewport_size(640,400);general.update_viewport();general.idle_wait_frames(60)
    result['captures']={};cases=list(camera_cases(json.loads(source)))
    for name,eye,direction,pose in cases:
        result['stage']=name;flush();view(eye,direction,pose);general.idle_wait_frames(30)
        signatures={}
        for label,enabled in (('textured',False),('decoded',True)):
            set_value(MATERIAL,enabled);general.idle_wait_frames(30)
            status=str(silpom.SilPomMeshRequestBus(bus.Event,'GetStatus',tower))
            if enabled:assert 'Level 4A material on' in status,status
            screenshot(OUTPUT/f'{name}-{label}.png')
            path=OUTPUT/f'{name}-{label}-hit.dds';capture_attachment(path,'Hit');signatures[label]=hit_signature(path)
        assert signatures['textured']==signatures['decoded'],'Material decode changed winning UV/depth/status'
        result['captures'][name]={'hit_signature':signatures['decoded'],'status':status};flush()
    # Motion and geometric-normal control remain independent of material decoding.
    name,eye,direction,pose=cases[0]
    for i in range(3):
        view((eye[0]+i*.003,eye[1]+i*.002,eye[2]),direction,pose);general.idle_wait_frames(5);screenshot(OUTPUT/f'motion-{i}.png')
    set_value(NORMAL,False);general.idle_wait_frames(10);screenshot(OUTPUT/'geometric-normal-off.png');set_value(NORMAL,originals[NORMAL])
    name,eye,direction,pose=cases[1];view(eye,direction,pose);general.idle_wait_frames(30)
    screenshot(OUTPUT/'light-on.png');set_value(BRIGHT,0.0);general.idle_wait_frames(20);screenshot(OUTPUT/'light-off.png')
    assert (OUTPUT/'light-on.png').read_bytes()!=(OUTPUT/'light-off.png').read_bytes(),'Dynamic light has no effect'
    set_value(BRIGHT,originals[BRIGHT]);general.idle_wait_frames(20)
    # Exercise a real runtime decal and the existing undo path on the decoded wall.
    canvas=silpom.DecalPainterBus(bus.Broadcast,'CreatePaintEntity')
    panel=next(w for w in QtWidgets.QApplication.allWidgets() if w.objectName()=='DecalPainter')
    viewport=QtWidgets.QWidget.find(silpom.DecalPainterBus(bus.Broadcast,'GetViewportWindowId'))
    panel.findChild(QtWidgets.QComboBox,'PaintMode').setCurrentIndex(0)
    panel.findChild(QtWidgets.QDoubleSpinBox,'BrushDiameter').setValue(.8)
    panel.findChild(QtWidgets.QDoubleSpinBox,'BrushOpacity').setValue(1.0)
    panel.findChild(QtWidgets.QCheckBox,'EnablePainting').setChecked(True)
    editor.ToolsApplicationRequestBus(bus.Broadcast,'SetSelectedEntities',[]);general.idle_wait_frames(30)
    screenshot(OUTPUT/'before-decal.png')
    pick=silpom.DecalPainterBus(bus.Broadcast,'PickVisiblePixel',320,200);assert pick.GetLength()>1
    local=QtCore.QPointF(320/viewport.devicePixelRatioF(),200/viewport.devicePixelRatioF())
    for kind,buttons in ((QtCore.QEvent.Type.MouseButtonPress,QtCore.Qt.MouseButton.LeftButton),(QtCore.QEvent.Type.MouseButtonRelease,QtCore.Qt.MouseButton.NoButton)):
        event=QtGui.QMouseEvent(kind,local,viewport.mapToGlobal(local),QtCore.Qt.MouseButton.LeftButton,buttons,QtCore.Qt.KeyboardModifier.NoModifier)
        QtWidgets.QApplication.sendEvent(viewport,event)
    general.idle_wait_frames(30);assert silpom.DecalPaintRequestBus(bus.Event,'GetDabCount',canvas)>0
    screenshot(OUTPUT/'after-decal.png');assert (OUTPUT/'before-decal.png').read_bytes()!=(OUTPUT/'after-decal.png').read_bytes()
    panel.findChild(QtWidgets.QCheckBox,'EnablePainting').setChecked(False)
    general.undo();general.idle_wait_frames(10);assert silpom.DecalPaintRequestBus(bus.Event,'GetDabCount',canvas)==0
    result['decal_and_undo']=True
    assert LEVEL.read_bytes()==source,'Saved level changed'
    result.update(passed=True,stage='complete')

try:
    flush();main()
except Exception:result['error']=traceback.format_exc()
finally:
    try:
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

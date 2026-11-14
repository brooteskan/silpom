"""Live lighting estimator switching; approved images/depth, no level saves."""
import hashlib
import json
import math
import os
from pathlib import Path
import time
import traceback

import azlmbr.atom as atom
import azlmbr.bus as bus
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.paths as paths
import azlmbr.silpom as silpom

PROJECT = Path(paths.projectroot)
LEVEL = PROJECT / "Levels/VueLiveLink/VueLiveLink.prefab"
OUTPUT = Path(os.environ["TG_SILPOM_ESTIMATOR_OUTPUT"])
OUTPUT.mkdir(parents=True, exist_ok=True)
VIEWPORT = (1280, 720)
result = {"passed": False, "stage": "startup", "variants": {}, "benchmark_performed": False}
originals, cvars = {}, {}
initial_view = {}
component = None
properties = {}


def flush():
    (OUTPUT / "results.json").write_text(json.dumps(result, indent=2), encoding="utf-8")


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
    assert actual.IsSuccess() and actual.GetValue() == value, label
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


def capture_hit(path):
    done=[]
    handler=atom.FrameCaptureNotificationBusHandler()
    response=atom.FrameCaptureRequestBus(bus.Broadcast, 'CapturePassAttachment', str(path),
        ['Root','MainPipeline_0','OpaquePass','SilPOMHitForward'], 'Hit', 1)
    assert response.IsSuccess(), str(response.GetError())
    handler.connect(response.GetValue())
    handler.add_callback('OnFrameCaptureFinished', lambda args: done.append(args[0]))
    try:
        deadline=time.monotonic()+45
        while not done and time.monotonic()<deadline:
            general.idle_wait_frames(1)
        assert done==[atom.FrameCaptureResult_Success], done
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


def main():
    global tower, component, properties
    source = LEVEL.read_bytes()
    result["level_sha256"] = hashlib.sha256(source).hexdigest()
    level = json.loads(source)
    general.idle_enable(True)
    assert general.open_level(str(LEVEL))
    general.idle_wait_frames(90)
    position, rotation = general.get_current_view_position(), general.get_current_view_rotation()
    size = general.get_viewport_size()
    initial_view.update(position=(position.x, position.y, position.z),
                        rotation=(rotation.x, rotation.y, rotation.z),
                        size=(int(size.x), int(size.y)),
                        policy=general.get_viewport_expansion_policy())
    result["initial_view"] = initial_view.copy()
    query = entity.SearchFilter()
    query.names = ["Irish Fieldstone Tower (Open Shaft)"]
    towers = entity.SearchBus(bus.Broadcast, "SearchEntities", query)
    assert len(towers) == 1
    tower = towers[0]
    ready()
    types = editor.EditorComponentAPIBus(bus.Broadcast, "FindComponentTypeIdsByEntityType",
                                        ["SilPOM Mesh"], entity.EntityType().Game)
    response = editor.EditorComponentAPIBus(bus.Broadcast, "GetComponentOfType", tower, types[0])
    assert response.IsSuccess()
    component = response.GetValue()
    available = editor.EditorComponentAPIBus(bus.Broadcast, "BuildComponentPropertyList", component)
    result["quality_controls"] = {}
    for path in available:
        if any(word in path.lower() for word in ("validation", "relief", "spiral", "normals", "level", "gather", "shadow", "r16")):
            response = editor.EditorComponentAPIBus(bus.Broadcast, "GetComponentProperty", component, path)
            if response.IsSuccess() and isinstance(response.GetValue(), (bool, int, float, str)):
                result["quality_controls"][path] = response.GetValue()
    labels = ["Lighting estimator", "Neural directional visibility (off = cheaper)", "Learned proxy directional response (off = cheaper)"]
    properties = {label: next(p for p in available if p.endswith("|" + label)) for label in labels}
    for label in labels:
        value = editor.EditorComponentAPIBus(bus.Broadcast, "GetComponentProperty", component, properties[label])
        assert value.IsSuccess()
        originals[label] = value.GetValue()
    result["original_settings"] = originals.copy()
    # Exercise relief in all three views instead of measuring the distant shell.
    # Preserve all saved quality settings.
    for key, value in (("ed_keepEditorActive", 1), ("ed_backgroundUpdatePeriod", -1),
                       ("vsync_interval", 0), ("sys_MaxFPS", -1), ("r_displayInfo", 0),
                       ("r_antiAliasing", 0)):
        cvars[key] = general.get_cvar(key)
        general.run_console(f"{key} {value}")
    general.close_pane("Console")
    general.set_viewport_expansion_policy("FixedSize")
    general.set_viewport_size(*VIEWPORT)
    general.update_viewport()
    general.idle_wait_frames(90)
    variants = [("current", 0), ("physical-ray", 1), ("field32", 2), ("caps4", 3), ("caps8", 4), ("restored", 0)]
    assert originals["Lighting estimator"] == 0, "Saved level must keep the current estimator as default"
    for label, estimator in variants:
        set_value("Lighting estimator", estimator)
        general.idle_wait_frames(30)
        folder = OUTPUT / label
        folder.mkdir(exist_ok=True)
        record = {"estimator": estimator, "views": {},
                  "status": str(silpom.SilPomMeshRequestBus(bus.Event, "GetStatus", tower))}
        result["variants"][label] = record
        for name, eye, direction, pose in camera_cases(level):
            result["stage"] = label + "/" + name
            flush()
            capture = view(eye, direction, pose)
            general.idle_wait_frames(90)
            screenshot(folder / (name + ".png"))
            capture_hit(folder / (name + "-hit.dds"))
            capture["proxy_state"] = str(silpom.SilPomMeshRequestBus(bus.Event, "GetProxyLodState", tower))
            if "glancing" in name and label in ("current", "restored"):
                for index in range(5):
                    moved = (eye[0] + .002 * index, eye[1] + .003 * index, eye[2] + .002 * index)
                    view(moved, direction, pose)
                    general.idle_wait_frames(4)
                    screenshot(folder / (name + f"-motion-{index}.png"))
                    if index in (1,4):
                        capture_hit(folder / (name + f"-motion-{index}-hit.dds"))
            record["views"][name] = capture
            flush()
        # Exercise the learned proxy under a distant footprint, without
        # changing LOD/depth controls or saving the temporary light switches.
        near="Neural directional visibility (off = cheaper)"
        proxy="Learned proxy directional response (off = cheaper)"
        set_value(near,True);set_value(proxy,True)
        view((60,0,12),(-1,0,0),pose);general.idle_wait_frames(45)
        record["far_proxy_state"]=str(silpom.SilPomMeshRequestBus(bus.Event,"GetProxyLodState",tower))
        owners=silpom.SilPomMeshRequestBus(bus.Event,"GetProxyFaceOwners",tower)
        assert owners,"Distant comparison must select proxy faces"
        screenshot(folder/"far-proxy.png");capture_hit(folder/"far-proxy-hit.dds")
        set_value(near,originals[near]);set_value(proxy,originals[proxy])
        flush()
    assert LEVEL.read_bytes() == source, "The saved level changed"
    result.update(passed=True, stage="complete", benchmark_performed=False)


try:
    flush()
    main()
except Exception:
    result["error"] = traceback.format_exc()
finally:
    try:
        if component is not None:
            for label, value in originals.items():
                set_value(label, value)
        for key, value in cvars.items():
            general.run_console(f"{key} {value}")
        if initial_view:
            general.set_current_view_position(*initial_view["position"])
            general.set_current_view_rotation(*initial_view["rotation"])
            # Avoid two layout rebuilds in the same tick. AutoExpand restores
            # its own size; only FixedSize needs an explicit dimension change.
            if general.get_viewport_expansion_policy() != initial_view["policy"]:
                general.set_viewport_expansion_policy(initial_view["policy"])
                general.idle_wait_frames(30)
            if initial_view["policy"] == "FixedSize":
                size = general.get_viewport_size()
                if (int(size.x), int(size.y)) != initial_view["size"]:
                    general.set_viewport_size(*initial_view["size"])
                    general.idle_wait_frames(30)
            general.idle_wait_frames(30)
        result["restored"] = True
    except Exception:
        result["restore_error"] = traceback.format_exc()
        result["passed"] = False
    flush()
    from PySide6 import QtCore
    QtCore.QTimer.singleShot(0, general.exit_no_prompt)

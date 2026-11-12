"""Fixed-camera GPU timestamps and images; never saves VueLiveLink."""
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
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
OUTPUT = Path(os.environ["TG_SILPOM_HEIGHT_AB_OUTPUT"])
OUTPUT.mkdir(parents=True, exist_ok=True)
SAMPLES = int(os.environ.get("TG_SILPOM_HEIGHT_AB_SAMPLES", "180"))
VIEWPORT = (1280, 720)
R16 = "R16 height textures (less GPU memory)"
LEVEL2 = "Level 2 predictor (experimental)"
ITER = "Validation iterations (lower = cheaper)"
RETRY = "Validation retry cap (lower = cheaper)"
TOL = "Validation tolerance (higher may be cheaper)"
GATHER = "Gather validation (may be cheaper)"
SHADOWS = "Spiral-light shadow samples (lower = cheaper)"
METHOD = "Local shadow method"
CONFIDENCE = "Hybrid shadow confidence (higher = safer)"
DISTANCE = "Maximum relief distance (closer = cheaper)"
DEPTH = "Relief depth multiplier"
result = {"passed": False, "stage": "startup", "samples": SAMPLES, "variants": {}}
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


def summary(values):
    assert values and statistics.median(values) > 0
    return {"median_ms": statistics.median(values),
            "p95_ms": sorted(values)[math.ceil(.95 * len(values)) - 1],
            "samples": len(values)}


def check_cancelled():
    if (OUTPUT/"cancel").exists():raise RuntimeError("Benchmark cancelled by request")


def measure(folder, name):
    check_cancelled()
    assert silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, "BeginCapture", SAMPLES)
    deadline = time.monotonic() + 180
    while not silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, "IsCaptureDone"):
        check_cancelled()
        assert time.monotonic() < deadline, "GPU timestamp capture timed out"
        general.idle_wait_frames(1)
    raw = json.loads(silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, "EndCapture"))
    assert raw["complete"] and raw["frames"] == SAMPLES
    assert (raw["depth_attachment"]["width"], raw["depth_attachment"]["height"]) == VIEWPORT
    (folder / (name + "-timestamps.json")).write_text(json.dumps(raw), encoding="utf-8")
    by_pass = {}
    silpom_frames = {}
    for frame, index, begin, ticks, ns in raw["samples"]:
        description = raw["passes"][index]
        if description["parent"] or ns <= 0:
            continue
        path = description["path"]
        by_pass.setdefault(path, []).append(ns / 1e6)
        if "silpom" in path.lower():
            silpom_frames[frame] = silpom_frames.get(frame, 0) + ns / 1e6
    return {"hardware": raw["hardware"], "depth_attachment": raw["depth_attachment"],
            "graphics_span": summary([v / 1e6 for v in raw["graphics_span_ns"] if v > 0]),
            "silpom_pass_sum": summary(list(silpom_frames.values())),
            "silpom_passes": {p: summary(v) for p, v in by_pass.items() if "silpom" in p.lower()}}


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
    recovery=os.environ.get("TG_SILPOM_RECOVER_VIEW")
    recovery_data=json.loads(Path(recovery).read_text()) if recovery else {}
    if recovery_data:
        initial_view.update(recovery_data["initial_view"])
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
        if any(word in path.lower() for word in ("validation", "relief", "spiral", "normals")):
            response = editor.EditorComponentAPIBus(bus.Broadcast, "GetComponentProperty", component, path)
            if response.IsSuccess() and isinstance(response.GetValue(), (bool, int, float, str)):
                result["quality_controls"][path] = response.GetValue()
    labels = [DISTANCE, R16, GATHER, LEVEL2, ITER, RETRY, TOL, SHADOWS, METHOD, CONFIDENCE, DEPTH]
    properties = {label: next(p for p in available if p.endswith("|" + label)) for label in labels}
    for label in labels:
        value = editor.EditorComponentAPIBus(bus.Broadcast, "GetComponentProperty", component, properties[label])
        assert value.IsSuccess()
        originals[label] = value.GetValue()
    result["original_settings"] = originals.copy()
    # Exercise relief in all three views instead of measuring the distant shell.
    for label,value in ((METHOD,0),(DEPTH,.5),(DISTANCE,0.0),(R16,False),(GATHER,False),(LEVEL2,False),
                        (ITER,6),(RETRY,4),(TOL,1.0),(CONFIDENCE,.995)):
        set_value(label,value)
    for key, value in (("ed_keepEditorActive", 1), ("ed_backgroundUpdatePeriod", -1),
                       ("vsync_interval", 0), ("sys_MaxFPS", -1), ("r_displayInfo", 0),
                       ("r_antiAliasing", 0)):
        cvars[key] = recovery_data.get("initial_cvars",{}).get(key,general.get_cvar(key))
        general.run_console(f"{key} {value}")
    result["initial_cvars"]=cvars.copy()
    general.close_pane("Console")
    general.set_viewport_expansion_policy("FixedSize")
    general.set_viewport_size(*VIEWPORT)
    general.update_viewport()
    general.idle_wait_frames(90)
    # Reuse the existing GPU query service with an invisible one-cell reference.
    # It contributes no draws; all variants use the same timestamp instrumentation.
    assert silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, "CreateReference", tower,
        "models/sphere_default_12399812254575482750.fbx.azmaterial", 1, .01, .01)
    deadline = time.monotonic() + 60
    while not silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, "IsReady"):
        assert time.monotonic() < deadline
        general.idle_wait_frames(1)
    result['stage']='validation';flush()
    result['comparison_relief_depth_multiplier']=.5
    if not math.isclose(originals[DEPTH],.5,abs_tol=1e-6):
        set_value(DEPTH,originals[DEPTH])
        response=editor.EditorComponentAPIBus(bus.Broadcast,"SetComponentProperty",component,properties[METHOD],3)
        assert response.IsSuccess()
        general.idle_wait_frames(10)
        status=str(silpom.SilPomMeshRequestBus(bus.Event,"GetStatus",tower))
        assert 'relief depth 0.5' in status and not silpom.SilPomMeshRequestBus(bus.Event,"IsReady",tower),status
        result['unsupported_depth_rejected']=status
        set_value(DEPTH,.5)
    # Apply the invalid schedule and method together, without waiting for an
    # intentionally rejected intermediate configuration to become ready.
    for label,value in ((SHADOWS,1),(METHOD,3)):
        response=editor.EditorComponentAPIBus(bus.Broadcast,"SetComponentProperty",component,properties[label],value)
        assert response.IsSuccess()
    general.idle_wait_frames(10)
    status=str(silpom.SilPomMeshRequestBus(bus.Event,"GetStatus",tower))
    assert '5-64' in status and not silpom.SilPomMeshRequestBus(bus.Event,"IsReady",tower),status
    result['unsupported_schedule_rejected']=status
    set_value(SHADOWS,9)
    variants = [("baseline-9",0,9),("gather-9",1,9),("hybrid-9",2,9),("neural-only-9",3,9),
                ("baseline-64",0,64),("gather-64",1,64),("neural-only-64",3,64),
                ("baseline-repeat-9",0,9)]
    def settle(seconds):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            check_cancelled();general.idle_wait_frames(1)
    for label,method,steps in variants:
        result['stage']='prewarm/'+label;flush()
        set_value(SHADOWS,steps);set_value(METHOD,method)
        for _,eye,direction,pose in camera_cases(level):
            view(eye,direction,pose);settle(.5)
    for label,method,steps in variants:
        folder=OUTPUT/label;folder.mkdir(exist_ok=True)
        set_value(SHADOWS,steps);set_value(METHOD,method);settle(3)
        record={"shadow_steps":steps,"method":method,"confidence":.995,"views":{},
                "status":str(silpom.SilPomMeshRequestBus(bus.Event,"GetStatus",tower))}
        result["variants"][label]=record
        for name, eye, direction, pose in camera_cases(level):
            result["stage"] = label + "/" + name
            flush()
            capture = view(eye, direction, pose)
            general.idle_wait_frames(90)
            settle(2)
            screenshot(folder / (name + ".png"))
            capture.update(measure(folder, name))
            if "glancing" in name:
                for index in range(5):
                    moved = (eye[0] + .002 * index, eye[1] + .003 * index, eye[2] + .002 * index)
                    view(moved, direction, pose)
                    general.idle_wait_frames(4)
                    screenshot(folder / (name + f"-motion-{index}.png"))
            record["views"][name] = capture
            flush()
    assert LEVEL.read_bytes() == source, "The saved level changed"
    result.update(passed=True, stage="complete")


try:
    flush()
    main()
except Exception:
    result["error"] = traceback.format_exc()
finally:
    try:
        silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, "Release")
        if component is not None:
            # Restore the complete original configuration before waiting. This
            # also recovers a configuration deliberately rejected by preflight.
            for label, value in originals.items():
                response=editor.EditorComponentAPIBus(bus.Broadcast,"SetComponentProperty",component,properties[label],value)
                assert response.IsSuccess(),label
            ready()
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

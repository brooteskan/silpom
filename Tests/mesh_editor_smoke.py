"""Exercise the real component controls and capture its imported mesh in Editor.

Run after create_mesh_fixture.py and Asset Processor, in a separate Editor with
--runpython. The test never saves or changes the user's DefaultLevel on disk.

This explicitly enables the bounded, experimental staged compute renderer.
It reads back real GPU hit states as well as capturing the rendered viewport.
See Docs/ImportedMeshIntegration.md for supported views and remaining limits.
"""
import json
import math
import os
import time
from pathlib import Path
import traceback
import struct
import sys
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
import azlmbr.settingsregistry as settingsregistry
sys.path.insert(0, str(Path(__file__).resolve().parent))
from mesh_capture_analysis import validate_depth, validate_albedo, validate_motion_frame, motion_frame_passes

verify_guard = os.environ.get("SILPOM_MESH_VERIFY_GUARD") == "1"
output = Path(paths.projectroot) / ("user/SilPOMMeshGuardValidation" if verify_guard else "user/SilPOMMeshStagedValidation")
if os.environ.get("SILPOM_MESH_OUTPUT"):
    output = Path(os.environ["SILPOM_MESH_OUTPUT"])
output.mkdir(parents=True, exist_ok=True)
result = {"passed": False, "captures": [], "stage": "startup", "guard_only": verify_guard}
original_size = None
original_policy = None
original_background = None
mesh = None
icons_key = "/Amazon/Preferences/Editor/IconsVisible"
registry = settingsregistry.g_SettingsRegistry
original_icons = registry.GetBool(icons_key)


def flush():
    (output / "result.json").write_text(json.dumps(result, indent=2))


def asset_id(path):
    value = asset.AssetCatalogRequestBus(bus.Broadcast, "GetAssetIdByPath", path, azmath.Uuid(), False)
    assert value.is_valid(), "Missing processed asset: " + path
    return value


def set_property(component, path, value):
    outcome = editor.EditorComponentAPIBus(bus.Broadcast, "SetComponentProperty", component, path, value)
    assert outcome.IsSuccess(), str(outcome.GetError())


def capture(name):
    started = time.perf_counter()
    general.idle_wait_frames(3)
    result.setdefault("three_frame_wall_ms", {})[name] = (time.perf_counter() - started) * 1000
    flush()
    filename = str(output / (name + ".png"))
    outcome = atom.FrameCaptureRequestBus(bus.Broadcast, "CaptureScreenshot", filename)
    assert outcome.IsSuccess()
    done = {}
    handler = atom.FrameCaptureNotificationBusHandler(); handler.connect(outcome.GetValue())
    handler.add_callback("OnFrameCaptureFinished", lambda args: done.update(success=args[0] == atom.FrameCaptureResult_Success))
    for _ in range(120):
        if done: break
        general.idle_wait_frames(1)
    handler.disconnect()
    assert done.get("success"), filename
    result["captures"].append(filename); flush()


def gpu_timestamps(name):
    done = {}
    handler = atom.ProfilingCaptureNotificationBusHandler(); handler.connect()
    handler.add_callback("OnCaptureQueryTimestampFinished", lambda args: done.update(success=bool(args[0]), info=str(args[1])))
    try:
        accepted = atom.ProfilingCaptureRequestBus(bus.Broadcast, "CapturePassTimestamp", str(output / (name + "_gpu.json")))
        for _ in range(120):
            if not accepted or done: break
            general.idle_wait_frames(1)
        result.setdefault("gpu_timestamps", {})[name] = {"accepted": bool(accepted), **done}; flush()
    finally:
        handler.disconnect()


def gpu_hits(name):
    filename = output / (name + "_hits.bin")
    outcome = atom.FrameCaptureRequestBus(bus.Broadcast, "CapturePassAttachment", str(filename),
                                         ["SilPomResolve"], "SilPomComputedHits", 1)
    assert outcome.IsSuccess(), "Cannot capture staged hit buffer"
    done = {}
    handler = atom.FrameCaptureNotificationBusHandler(); handler.connect(outcome.GetValue())
    handler.add_callback("OnFrameCaptureFinished", lambda args: done.update(success=args[0] == atom.FrameCaptureResult_Success))
    for _ in range(120):
        if done: break
        general.idle_wait_frames(1)
    handler.disconnect()
    assert done.get("success"), "Hit buffer readback failed"
    data = filename.read_bytes()
    reports = []
    for base in silpom.SilPomMeshRequestBus(bus.Event, "GetHitOffsets", mesh):
        count = struct.unpack_from("<I", data, base)[0]
        assert 0 < count <= 32
        for view in range(count):
            header = base + 16 + view * 128
            width, height, near_z, error = struct.unpack_from("<4f", data, header + 64)
            left, top, columns, rows = struct.unpack_from("<4I", data, header + 80)
            offset = struct.unpack_from("<I", data, header + 96)[0]
            statuses = [0, 0, 0, 0]
            max_error = 0.0
            if not error:
                for sample in range(columns * rows):
                    address = offset + sample * 96
                    status = struct.unpack_from("<I", data, address + 12)[0]
                    assert 0 <= status <= 3, "Invalid GPU record layout"
                    statuses[status] += 1
                    if status == 1:
                        position = struct.unpack_from("<3f", data, address)
                        normal = struct.unpack_from("<3f", data, address + 16)
                        uv_error = struct.unpack_from("<3f", data, address + 32)
                        depth = struct.unpack_from("<f", data, address + 64)[0]
                        assert all(math.isfinite(v) for v in position + normal + uv_error + (depth,))
                        assert 0 <= depth <= 1
                        assert abs(sum(v*v for v in normal)-1.0) < 0.001
                        assert 0 <= uv_error[2] <= 0.0005
                        max_error = max(max_error, uv_error[2])
            reports.append(dict(batch_offset=base, view=view, shadow=near_z == 0,
                                viewport=[width, height], rectangle=[left, top, columns, rows],
                                view_error=error, miss=statuses[0], hit=statuses[1],
                                exhausted=statuses[2], invalid=statuses[3], max_t_error=max_error))
    result.setdefault("gpu_hits", {})[name] = reports; flush()
    assert sum(v["hit"] for v in reports if not v["shadow"]) > 0, "No camera hits"
    assert sum(v["hit"] for v in reports if v["shadow"]) > 0, "No shadow hits"
    assert not any(v["view_error"] or v["exhausted"] or v["invalid"] for v in reports), "Uncertified GPU samples"


def depth_capture(name, shadow=False):
    filename = str(output / (name + ("_shadow.dds" if shadow else "_depth.dds")))
    hierarchy = ["MainPipeline_0", "Shadows", "Cascades"] if shadow else ["MainPipeline_0", "DepthPrePass"]
    attachment = "Shadowmap" if shadow else "Depth"
    outcome = atom.FrameCaptureRequestBus(bus.Broadcast, "CapturePassAttachment", filename,
                                         hierarchy, attachment, 1)
    assert outcome.IsSuccess(), "Cannot capture " + attachment
    done = {}
    handler = atom.FrameCaptureNotificationBusHandler(); handler.connect(outcome.GetValue())
    handler.add_callback("OnFrameCaptureFinished", lambda args: done.update(success=args[0] == atom.FrameCaptureResult_Success))
    for _ in range(120):
        if done: break
        general.idle_wait_frames(1)
    handler.disconnect()
    assert done.get("success"), attachment + " readback failed"
    result.setdefault("shadow_captures" if shadow else "depth_captures", {})[name] = filename; flush()
    check = validate_depth(output, name, result["gpu_hits"][name], shadow=shadow)
    result.setdefault("shadow_checks" if shadow else "depth_checks", {})[name] = check; flush()
    assert check["matching"] and not check["missing_or_farther"] and check["max_projection_error"] < 1e-5, check


def albedo_capture(name):
    filename = str(output / (name + "_albedo.dds"))
    outcome = atom.FrameCaptureRequestBus(bus.Broadcast, "CapturePassAttachment", filename,
                                         ["MainPipeline_0", "OpaquePass", "Forward"], "AlbedoOutput", 1)
    assert outcome.IsSuccess(), "Cannot capture forward albedo"
    done = {}
    handler = atom.FrameCaptureNotificationBusHandler(); handler.connect(outcome.GetValue())
    handler.add_callback("OnFrameCaptureFinished", lambda args: done.update(success=args[0] == atom.FrameCaptureResult_Success))
    for _ in range(120):
        if done: break
        general.idle_wait_frames(1)
    handler.disconnect()
    assert done.get("success"), "Forward albedo readback failed"
    result.setdefault("albedo_captures", {})[name] = filename; flush()
    check = validate_albedo(output, name, result["gpu_hits"][name])
    result.setdefault("albedo_checks", {})[name] = check; flush()
    assert check["red"] + check["blue"] and not check["wrong_or_missing"], check


def moving_camera_captures():
    # Keep changing the camera on EVERY TickBus tick, including the frames
    # between requesting a GPU capture and receiving its async readback. A
    # settled-camera capture would miss a one-frame-stale view directory.
    motion = {"ticks": 0}
    def on_tick(args):
        motion["ticks"] += 1
        phase = motion["ticks"] * 0.09
        general.set_current_view_position(16.0 + 0.25 * math.sin(phase), 11.0, 40.0)
        general.set_current_view_rotation(0.0, 0.0, 3.0 * math.cos(phase))
    ticker = components.TickBusHandler()
    ticker.connect()
    ticker.add_callback("OnTick", on_tick)
    checks = []
    try:
        general.idle_wait_frames(5)
        assert motion["ticks"] >= 5, "Camera animation did not tick"
        for sample in range(12):
            filename = output / ("motion_%02d_albedo.dds" % sample)
            before = motion["ticks"]
            outcome = atom.FrameCaptureRequestBus(bus.Broadcast, "CapturePassAttachment", str(filename),
                ["MainPipeline_0", "OpaquePass", "Forward"], "AlbedoOutput", 1)
            assert outcome.IsSuccess(), "Cannot capture moving-camera albedo"
            done = {}
            handler = atom.FrameCaptureNotificationBusHandler(); handler.connect(outcome.GetValue())
            handler.add_callback("OnFrameCaptureFinished", lambda args: done.update(success=args[0] == atom.FrameCaptureResult_Success))
            hit_handler = None
            hit_done = {}
            try:
                # Queue both copies before yielding so diagnostics can be tied
                # to the exact same frame's certified/exhausted GPU statuses.
                hit_path = output / ("motion_%02d_hits.bin" % sample)
                hit_outcome = atom.FrameCaptureRequestBus(bus.Broadcast, "CapturePassAttachment", str(hit_path),
                    ["SilPomResolve"], "SilPomComputedHits", 1)
                assert hit_outcome.IsSuccess(), "Cannot capture moving-camera hit records"
                hit_handler = atom.FrameCaptureNotificationBusHandler(); hit_handler.connect(hit_outcome.GetValue())
                hit_handler.add_callback("OnFrameCaptureFinished", lambda args: hit_done.update(success=args[0] == atom.FrameCaptureResult_Success))
                for _ in range(120):
                    if done and hit_done: break
                    general.idle_wait_frames(1)
            finally:
                handler.disconnect()
                if hit_handler is not None: hit_handler.disconnect()
            assert done.get("success"), "Moving-camera readback failed"
            assert hit_done.get("success"), "Moving-camera hit readback failed"
            offsets = list(silpom.SilPomMeshRequestBus(bus.Event, "GetHitOffsets", mesh))
            check = validate_motion_frame(filename, hit_path, offsets)
            check.update(capture=str(filename), moving_ticks=motion["ticks"] - before, start_tick=before)
            check.update(hit_capture=str(hit_path), batch_offsets=offsets)
            checks.append(check)
            result["motion_checks"] = checks; flush()
            assert check["moving_ticks"] > 0, "Camera stopped during capture"
        assert all(motion_frame_passes(check) for check in checks), checks
    finally:
        ticker.disconnect()
        general.set_current_view_position(16.0, 11.0, 40.0)
        general.set_current_view_rotation(0.0, 0.0, 0.0)


try:
    flush()
    registry.SetBool(icons_key, False)
    general.idle_enable(True)
    original_background = general.get_cvar("ed_backgroundUpdatePeriod")
    general.run_console("ed_backgroundUpdatePeriod -1")
    assert general.open_level("DefaultLevel")
    general.idle_wait_frames(30)
    # MainRenderPipeline defaults to 2x MSAA. The initial staged contract is
    # single-sample; this cvar affects only this disposable Editor process.
    general.run_console("r_multiSampleCount 1")
    general.run_console("r_displayInfo 0")
    general.idle_wait_frames(5)
    result["msaa_samples"] = 1
    # Bound the first shader workload in both camera and light views. These
    # changes affect only this unsaved test session, not the source level.
    render.DirectionalLightRequestBus(bus.Broadcast, "SetShadowmapSize", 256)
    render.DirectionalLightRequestBus(bus.Broadcast, "SetCascadeCount", 1)
    render.DirectionalLightRequestBus(bus.Broadcast, "SetShadowFarClipDistance", 10.0)
    result["shadow_settings"] = {"resolution": 256, "cascades": 1, "far_clip_metres": 10.0}
    viewport = general.get_viewport_size(); original_size = (int(viewport.x), int(viewport.y))
    original_policy = general.get_viewport_expansion_policy()
    general.set_viewport_expansion_policy("FixedSize")
    # Start with a bounded pixel workload while integrating the expensive kernel.
    general.set_viewport_size(160, 120); general.update_viewport()
    mesh = editor.ToolsApplicationRequestBus(bus.Broadcast, "CreateNewEntity", entity.EntityId())
    editor.EditorEntityAPIBus(bus.Event, "SetName", mesh, "SilPOM_Mesh_Integration")
    types = editor.EditorComponentAPIBus(bus.Broadcast, "FindComponentTypeIdsByEntityType", ["SilPOM Mesh"], entity.EntityType().Game)
    added = editor.EditorComponentAPIBus(bus.Broadcast, "AddComponentsOfType", mesh, types)
    assert added.IsSuccess(), added.GetError()
    component = added.GetValue()[0]
    set_property(component, "Controller|Configuration|Enable experimental raster", not verify_guard)
    set_property(component, "Controller|Configuration|Mesh surface", asset_id("assets/silpommeshintegration/curved.fbx.silpommesh"))
    components.TransformBus(bus.Event, "SetWorldTranslation", mesh, azmath.Vector3(16.0, 16.0, 40.0))
    components.TransformBus(bus.Event, "SetWorldRotationQuaternion", mesh, azmath.Quaternion_CreateRotationX(math.pi / 2))
    general.set_current_view_position(16.0, 11.0, 40.0); general.set_current_view_rotation(0.0, 0.0, 0.0)
    for _ in range(300):
        status = silpom.SilPomMeshRequestBus(bus.Event, "GetStatus", mesh)
        if "discovered" in status or "Assign material" in status: break
        general.idle_wait_frames(1)
    properties = editor.EditorComponentAPIBus(bus.Broadcast, "BuildComponentPropertyList", component)
    result.update(stage="binding", properties=list(properties), status=status); flush()
    fixture = json.loads((Path(paths.projectroot) / "Assets/SilPOMMeshIntegration/fixture.json").read_text())
    for path in properties:
        if path.endswith("|Authored material ID"):
            identity = editor.EditorComponentAPIBus(bus.Broadcast, "GetComponentProperty", component, path)
            assert identity.IsSuccess()
            filename = fixture["materials"][identity.GetValue()].replace(".material", ".azmaterial")
            set_property(component, path.rsplit("|", 1)[0] + "|Material", asset_id("assets/silpommeshintegration/" + filename))
        elif path.endswith("|Height image"):
            set_property(component, path, asset_id("assets/silpommeshintegration/height.png.streamingimage"))
    for _ in range(600):
        status = silpom.SilPomMeshRequestBus(bus.Event, "GetStatus", mesh)
        if result.get("status") != status:
            result["status"] = status; flush()
        if verify_guard and status.startswith("Experimental raster disabled"): break
        if silpom.SilPomMeshRequestBus(bus.Event, "IsReady", mesh): break
        general.idle_wait_frames(1)
    result["status"] = silpom.SilPomMeshRequestBus(bus.Event, "GetStatus", mesh); flush()
    if verify_guard:
        assert not silpom.SilPomMeshRequestBus(bus.Event, "IsReady", mesh)
        assert result["status"].startswith("Experimental raster disabled"), result["status"]
        result["stage"] = "guard verified; renderer acceptance not run"
    else:
        assert silpom.SilPomMeshRequestBus(bus.Event, "IsReady", mesh), result["status"]
        editor.ToolsApplicationRequestBus(bus.Broadcast, "SetSelectedEntities", [])
        result["stage"] = "rendering"; flush()
        moving_camera_captures()
        general.idle_wait_frames(3)
        gpu_hits("front")
        depth_capture("front")
        albedo_capture("front")
        depth_capture("front", shadow=True)
        gpu_timestamps("front")
        capture("front")
        general.set_current_view_position(19.0, 15.47, 40.0); general.set_current_view_rotation(0.0, 0.0, 80.0)
        general.idle_wait_frames(3)
        gpu_hits("grazing")
        depth_capture("grazing")
        albedo_capture("grazing")
        depth_capture("grazing", shadow=True)
        gpu_timestamps("grazing")
        capture("grazing")
    result["passed"] = True
except Exception:
    result["error"] = traceback.format_exc()
finally:
    if original_icons is None: registry.RemoveKey(icons_key)
    else: registry.SetBool(icons_key, original_icons)
    flush(); print(json.dumps(result, indent=2))
    if mesh is not None:
        editor.ToolsApplicationRequestBus(bus.Broadcast, "DeleteEntityById", mesh)
        general.idle_wait_frames(2)
    if original_size:
        general.set_viewport_size(*original_size); general.set_viewport_expansion_policy(original_policy)
    if original_background is not None: general.run_console("ed_backgroundUpdatePeriod " + str(original_background))
    general.exit_no_prompt()

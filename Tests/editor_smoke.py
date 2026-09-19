"""Run via Editor --runpython. Creates an unsaved fixture in a separate Editor session."""
import json
import math
import os
import traceback
from pathlib import Path
import azlmbr.asset as asset
import azlmbr.atom as atom
import azlmbr.bus as bus
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.math as azmath
import azlmbr.paths as paths
import azlmbr.silpom as silpom
import azlmbr.components as components

output = Path(os.environ.get("SILPOM_CAPTURE_DIR", str(Path(paths.projectroot) / "user/SilPOMValidation")))
output.mkdir(parents=True, exist_ok=True)
result = {"passed": False, "captures": []}
original_viewport = None
original_policy = None
original_background = None


def screenshot(name):
    filename = str(output / (name + ".png"))
    general.idle_wait_frames(60)
    outcome = atom.FrameCaptureRequestBus(bus.Broadcast, "CaptureScreenshot", filename)
    assert outcome.IsSuccess(), "Screenshot request failed"
    done = {}
    handler = atom.FrameCaptureNotificationBusHandler()
    handler.connect(outcome.GetValue())
    handler.add_callback("OnFrameCaptureFinished", lambda args: done.update(success=args[0] == atom.FrameCaptureResult_Success))
    for _ in range(300):
        if done:
            break
        general.idle_wait_frames(1)
    handler.disconnect()
    assert done.get("success"), filename
    result["captures"].append(filename)


def gpu_timestamps(name):
    done = {}
    handler = atom.ProfilingCaptureNotificationBusHandler()
    handler.connect()
    handler.add_callback("OnCaptureQueryTimestampFinished", lambda args: done.update(success=bool(args[0]), info=str(args[1])))
    try:
        accepted = atom.ProfilingCaptureRequestBus(bus.Broadcast, "CapturePassTimestamp", str(output / (name + "_gpu.json")))
        for _ in range(600):
            if not accepted or done:
                break
            general.idle_wait_frames(1)
        result.setdefault("gpu_timestamps", {})[name] = {"accepted": bool(accepted), **done}
    finally:
        handler.disconnect()


try:
    general.idle_enable(True)
    original_background = general.get_cvar("ed_backgroundUpdatePeriod")
    general.run_console("ed_backgroundUpdatePeriod -1")
    assert general.open_level("DefaultLevel")
    general.idle_wait(5.0)
    patch = editor.ToolsApplicationRequestBus(bus.Broadcast, "CreateNewEntity", entity.EntityId())
    editor.EditorEntityAPIBus(bus.Event, "SetName", patch, "SilPOM_Validation")
    types = editor.EditorComponentAPIBus(bus.Broadcast, "FindComponentTypeIdsByEntityType", ["SilPOM Patch"], entity.EntityType().Game)
    assert len(types) == 1
    added = editor.EditorComponentAPIBus(bus.Broadcast, "AddComponentsOfType", patch, types)
    assert added.IsSuccess(), added.GetError()
    component = added.GetValue()[0]
    material = asset.AssetCatalogRequestBus(bus.Broadcast, "GetAssetIdByPath", "assets/silpom/stonewall.azmaterial", azmath.Uuid(), False)
    assert material.is_valid(), "Fixture material was not processed"
    outcome = editor.EditorComponentAPIBus(bus.Broadcast, "SetComponentProperty", component, "Controller|Configuration|Material", material)
    assert outcome.IsSuccess(), outcome.GetError()
    # A vertical wall above terrain isolates silhouette/coverage from underlying walls.
    components.TransformBus(bus.Event, "SetWorldTranslation", patch, azmath.Vector3(16.0, 16.0, 40.0))
    components.TransformBus(bus.Event, "SetWorldRotationQuaternion", patch, azmath.Quaternion_CreateRotationX(math.pi / 2))
    for _ in range(600):
        if silpom.SilPomPatchRequestBus(bus.Event, "IsReady", patch):
            break
        general.idle_wait_frames(1)
    result["status"] = silpom.SilPomPatchRequestBus(bus.Event, "GetStatus", patch)
    assert silpom.SilPomPatchRequestBus(bus.Event, "IsReady", patch), result["status"]
    editor.ToolsApplicationRequestBus(bus.Broadcast, "SetSelectedEntities", [])
    viewport = general.get_viewport_size()
    original_viewport = (int(viewport.x), int(viewport.y))
    original_policy = general.get_viewport_expansion_policy()
    general.set_viewport_expansion_policy("FixedSize")
    general.set_viewport_size(1280, 720)
    general.update_viewport()
    for name, position, rotation in [
        ("front", (16, 13, 40), (0, 0, 0)),
        ("edge_left", (16, 13, 40), (0, 0, -45)),
        ("edge_right", (16, 13, 40), (0, 0, 45)),
        ("edge_top", (16, 13, 40), (-25, 0, 0)),
        ("edge_bottom", (16, 13, 40), (25, 0, 0)),
        ("grazing80", (19, 15.471, 40), (0, 0, 80)),
        ("grazing85", (19, 15.738, 40), (0, 0, 85)),
        ("grazing88", (19, 15.895, 40), (0, 0, 88)),
        # The Editor's default near plane is 0.2 m: it slices the displacement envelope here.
        ("near_plane", (16, 15.8, 40), (0, 0, 0)),
        # Look along the wall while the eye is inside its conservative volume.
        ("inside", (16, 15.99, 40), (0, 0, -88)),
    ]:
        general.set_current_view_position(*map(float, position))
        general.set_current_view_rotation(*map(float, rotation))
        screenshot(name)
        if name in ("front", "grazing85", "inside"):
            gpu_timestamps(name)
            assert editor.EditorComponentAPIBus(bus.Broadcast, "DisableComponents", [component])
            general.idle_wait_frames(60)
            gpu_timestamps(name + "_without_patch")
            assert editor.EditorComponentAPIBus(bus.Broadcast, "EnableComponents", [component])
            general.idle_wait_frames(60)
            assert silpom.SilPomPatchRequestBus(bus.Event, "IsReady", patch)
    # Repeated activation catches stale handles and asynchronous packet recreation.
    for _ in range(5):
        assert editor.EditorComponentAPIBus(bus.Broadcast, "DisableComponents", [component])
        general.idle_wait_frames(3)
        assert editor.EditorComponentAPIBus(bus.Broadcast, "EnableComponents", [component])
        general.idle_wait_frames(60)
        assert silpom.SilPomPatchRequestBus(bus.Event, "IsReady", patch)
    result["passed"] = True
except Exception:
    result["error"] = traceback.format_exc()
finally:
    if original_viewport:
        general.set_viewport_size(*original_viewport)
        general.set_viewport_expansion_policy(original_policy)
    if original_background is not None:
        general.run_console("ed_backgroundUpdatePeriod " + str(original_background))
    (output / "result.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))
    general.idle_wait_frames(120)
    general.exit_no_prompt()

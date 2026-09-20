"""Editor --runpython: unsaved, full-resolution planar A/B fixture.

Generate Assets/SilPOMPlanarAB first. Environment overrides are recorded, never
silently accepted as the final workload. See Docs/PlanarAB.md for reproduction.
"""
import json
import hashlib
import math
import os
import platform
import time
import traceback
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
import azlmbr.silpom as silpom

project = Path(paths.projectroot)
manifest = json.loads((project / "Assets/SilPOMPlanarAB/manifest.json").read_text())
output = Path(os.environ.get("SILPOM_PLANAR_OUTPUT", str(
    project / "user/SilPOMPlanarAB" / time.strftime("%Y%m%d-%H%M%S"))))
output.mkdir(parents=True, exist_ok=False)
samples = int(os.environ.get("SILPOM_PLANAR_SAMPLES", manifest["samples"]))
runs = int(os.environ.get("SILPOM_PLANAR_RUNS", manifest["runs"]))
grids = [int(x) for x in os.environ.get("SILPOM_PLANAR_GRIDS", "256").split(",")]
views = os.environ.get("SILPOM_PLANAR_VIEWS", "front,oblique,grazing,moving").split(",")
result = {"passed": False, "manifest": manifest, "platform": platform.platform(),
          "samples": samples, "runs": runs, "grids": grids, "views": views,
          "captures": [], "timings": [], "errors": [], "camera_radius_metres": 3,
          "quad_vertices": 4, "quad_triangles": 2, "overlay_enabled": True,
          "thresholds_agreed": manifest["thresholds"]["agreed"]}
original_cvars = {}
original_viewport = original_policy = None
patch = None
level_name = os.environ.get("SILPOM_PLANAR_LEVEL", "")
level_path = project / "Levels" / level_name / (level_name + ".prefab")
assert level_name.startswith("SilPOMPlanarAB_"), "Use a disposable snapshot; see Tools/prepare_planar_ab_level.py"
level_hash = hashlib.sha256(level_path.read_bytes()).hexdigest()
result["level"] = str(level_path)
result["level_sha256"] = level_hash
result["shader_sha256"] = {name: hashlib.sha256((Path(__file__).parent.parent / name).read_bytes()).hexdigest()
                           for name in ("Assets/Shaders/SilPOM/Heightfield.azsli",
                                        "Assets/Materials/Types/SilPOM/SilPOM.azsli")}


def benchmark(event, *args):
    return silpom.SilPomPlanarBenchmarkBus(bus.Broadcast, event, *args)


def checkpoint():
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")


def wait_until(predicate, frames=1200):
    for _ in range(frames):
        if predicate():
            return
        general.idle_wait_frames(1)
    raise TimeoutError("Timed out waiting for editor/GPU")


def camera(view, phase=0):
    # Same deterministic positions for both variants, including camera motion.
    angle = {"front": 0, "oblique": 60, "grazing": 85, "moving": 0}[view]
    if view == "moving":
        angle = 82 * math.sin(2 * math.pi * phase / 600)
    radians = math.radians(angle)
    general.set_current_view_position(16 + 3 * math.sin(radians), 16 - 3 * math.cos(radians), 40.0)
    general.set_current_view_rotation(0.0, 0.0, float(angle))


def capture(name, pass_path=None, slot=None):
    filename = output / (name + (".dds" if pass_path else ".png"))
    if pass_path:
        outcome = atom.FrameCaptureRequestBus(bus.Broadcast, "CapturePassAttachment",
                                              str(filename), pass_path, slot, 1)
    else:
        outcome = atom.FrameCaptureRequestBus(bus.Broadcast, "CaptureScreenshot", str(filename))
    assert outcome.IsSuccess(), "Capture request failed: " + name
    done = {}
    handler = atom.FrameCaptureNotificationBusHandler()
    handler.connect(outcome.GetValue())
    handler.add_callback("OnFrameCaptureFinished", lambda args: done.update(
        success=args[0] == atom.FrameCaptureResult_Success, info=str(args[1])))
    try:
        wait_until(lambda: bool(done), 300)
        assert done["success"], done
    finally:
        handler.disconnect()
    result["captures"].append({"name": name, "path": str(filename), "pass": pass_path, "slot": slot})
    checkpoint()


def property_value(component, name, value):
    outcome = editor.EditorComponentAPIBus(bus.Broadcast, "SetComponentProperty", component,
                                          "Controller|Configuration|" + name, value)
    assert outcome.IsSuccess(), name + ": " + str(outcome.GetError())


def depth_interaction():
    # Ordinary (non-displaced) scene mesh crossing the relief envelope. Captured
    # separately: the isolated-wall ROI segmentation is not valid for this case.
    cube = editor.ToolsApplicationRequestBus(bus.Broadcast, "CreateNewEntity", entity.EntityId())
    editor.EditorEntityAPIBus(bus.Event, "SetName", cube, "SilPOM_Occlusion_Box_UNSAVED")
    types = editor.EditorComponentAPIBus(bus.Broadcast, "FindComponentTypeIdsByEntityType",
                                       ["Mesh"], entity.EntityType().Game)
    added = editor.EditorComponentAPIBus(bus.Broadcast, "AddComponentsOfType", cube, types)
    assert added.IsSuccess(), added.GetError()
    model = asset.AssetCatalogRequestBus(bus.Broadcast, "GetAssetIdByPath",
        "materialeditor/viewportmodels/cube.fbx.azmodel", azmath.Uuid(), False)
    assert model.is_valid(), "Ordinary cube fixture asset missing"
    property_value(added.GetValue()[0], "Model Asset", model)
    components.TransformBus(bus.Event, "SetWorldTranslation", cube, azmath.Vector3(16.6, 15.97, 40.0))
    components.TransformBus(bus.Event, "SetLocalUniformScale", cube, .2)
    editor.ToolsApplicationRequestBus(bus.Broadcast, "SetSelectedEntities", [])
    for view in ("front", "oblique", "grazing"):
        camera(view)
        for variant in ("quad", "reference"):
            benchmark("SetReferenceVisible", variant == "reference")
            benchmark("SetOverlay", f"Planar A/B: ordinary-geometry occlusion | {variant} | {view}")
            general.idle_wait_frames(120)
            capture(f"occlusion_{view}_{variant}")
            capture(f"occlusion_{view}_{variant}_depth", ["MainPipeline_0", "DepthPrePass"], "DepthLinear")


def motion_keyframes(cells):
    # Separate from timed runs: readback stalls must not bias GPU measurements.
    for variant in ("quad", "reference"):
        benchmark("SetReferenceVisible", variant == "reference")
        benchmark("SetOverlay", f"Planar A/B: moving camera | {variant} | grid {cells}")
        camera("moving", 0)
        general.idle_wait_frames(120)
        for phase in range(601):
            camera("moving", phase)
            general.idle_wait_frames(1)
            if phase % 75 == 0:
                capture(f"grid{cells}_motion_{variant}_{phase:03}")


try:
    general.idle_enable(True)
    for name, value in {"ed_backgroundUpdatePeriod": -1}.items():
        original_cvars[name] = general.get_cvar(name)
        general.run_console(name + " " + str(value))
    assert general.open_level(level_name)
    general.idle_wait(5.0)
    viewport = general.get_viewport_size()
    original_viewport = (int(viewport.x), int(viewport.y))
    original_policy = general.get_viewport_expansion_policy()
    general.set_viewport_expansion_policy("FixedSize")
    general.set_viewport_size(*manifest["resolution"])
    general.update_viewport()
    general.idle_wait_frames(30)
    # Atom's render pipeline must exist before applying these callbacks.
    for name, value in {"r_renderScale": 1.0, "vsync_interval": 0}.items():
        original_cvars[name] = general.get_cvar(name)
        general.run_console(name + " " + str(value))
    general.idle_wait_frames(30)
    patch = editor.ToolsApplicationRequestBus(bus.Broadcast, "CreateNewEntity", entity.EntityId())
    editor.EditorEntityAPIBus(bus.Event, "SetName", patch, "SilPOM_Planar_AB_UNSAVED")
    types = editor.EditorComponentAPIBus(bus.Broadcast, "FindComponentTypeIdsByEntityType",
                                       ["SilPOM Patch"], entity.EntityType().Game)
    added = editor.EditorComponentAPIBus(bus.Broadcast, "AddComponentsOfType", patch, types)
    assert added.IsSuccess(), added.GetError()
    component = added.GetValue()[0]
    material = asset.AssetCatalogRequestBus(bus.Broadcast, "GetAssetIdByPath",
                                           manifest["quad_material"], azmath.Uuid(), False)
    assert material.is_valid(), "Process the planar A/B assets first"
    for name, value in {
        "Width": manifest["width_metres"], "Height": manifest["height_metres"],
        "Height scale (world metres)": manifest["scale_metres"],
        "Reference height": manifest["midpoint"], "Maximum cells": manifest["max_cells"],
        "Material": material,
    }.items():
        property_value(component, name, value)
    components.TransformBus(bus.Event, "SetWorldTranslation", patch, azmath.Vector3(16.0, 16.0, 40.0))
    components.TransformBus(bus.Event, "SetWorldRotationQuaternion", patch,
                            azmath.Quaternion_CreateRotationX(math.pi / 2))
    wait_until(lambda: silpom.SilPomPatchRequestBus(bus.Event, "IsReady", patch))
    result["status"] = silpom.SilPomPatchRequestBus(bus.Event, "GetStatus", patch)
    editor.ToolsApplicationRequestBus(bus.Broadcast, "SetSelectedEntities", [])
    result["cvars"] = {name: general.get_cvar(name) for name in original_cvars}
    size = general.get_viewport_size()
    result["actual_resolution"] = [int(size.x), int(size.y)]
    assert result["actual_resolution"] == manifest["resolution"], "Viewport is not native target resolution"
    assert float(result["cvars"]["r_renderScale"]) == 1.0, "Reduced resolution is forbidden"
    checkpoint()
    for cells in grids:
        assert benchmark("CreateReference", patch, manifest["reference_material"], cells,
                         manifest["width_metres"], manifest["height_metres"])
        wait_until(lambda: benchmark("IsReady"))
        for view in views:
            for run in range(runs):
                # Alternate run order to expose thermal/order bias.
                variants = ("quad", "reference") if run % 2 == 0 else ("reference", "quad")
                for variant in variants:
                    name = f"grid{cells}_{view}_{variant}_run{run}"
                    benchmark("SetReferenceVisible", variant == "reference")
                    benchmark("SetOverlay", f"SilPOM planar A/B | {variant} | {view} | grid {cells}\n"
                              f"Native {manifest['resolution']} MSAA {manifest['msaa']} | flat shadows | POM off\n"
                              "Magenta = exhausted; yellow = invalid | no hardware RT")
                    camera(view)
                    general.idle_wait_frames(manifest["warmup_frames"])
                    assert benchmark("BeginCapture", samples), "Timestamp capture unavailable"
                    for frame in range(samples * 4 + 600):
                        if benchmark("IsCaptureDone"):
                            break
                        if view == "moving":
                            camera(view, frame)
                        general.idle_wait_frames(1)
                    timing = json.loads(benchmark("EndCapture"))
                    assert hashlib.sha256(level_path.read_bytes()).hexdigest() == level_hash, "Scene changed during benchmark"
                    position = components.TransformBus(bus.Event, "GetWorldTranslation", patch)
                    assert abs(position.x-16) < 1e-5 and abs(position.y-16) < 1e-5 and abs(position.z-40) < 1e-5, "Test patch moved during capture"
                    timing.update(variant=variant, view=view, run=run, cells=cells,
                                  triangles=2 if variant == "quad" else 2 * cells * cells)
                    (output / (name + "_gpu.json")).write_text(json.dumps(timing))
                    result["timings"].append(name + "_gpu.json")
                    checkpoint()
                    assert timing["complete"] and timing["frames"] == samples, "Incomplete GPU readback"
                    if "depth_attachment" in timing:
                        assert timing["depth_attachment"] == {
                            "width": manifest["resolution"][0], "height": manifest["resolution"][1],
                            "msaa": manifest["msaa"]}, "Actual depth target does not match workload"
                    if run == 0:
                        camera(view)
                        general.idle_wait_frames(manifest["warmup_frames"])
                        capture(name)
                        capture(name + "_depth", ["MainPipeline_0", "DepthPrePass"], "DepthLinear")
                        if view == "front":
                            capture(name + "_shadow", ["MainPipeline_0", "Shadows", "Cascades"], "Shadowmap")
            if view == "moving":
                motion_keyframes(cells)
            # Capture diagnostic appearance separately from normal shaded timings.
            benchmark("SetReferenceVisible", False)
            silpom.SilPomPatchRequestBus(bus.Event, "SetDebug", patch, 5)
            camera(view)
            general.idle_wait_frames(30)
            capture(f"grid{cells}_{view}_hit_mask")
            silpom.SilPomPatchRequestBus(bus.Event, "SetDebug", patch, 0)
    if os.environ.get("SILPOM_PLANAR_OCCLUSION", "1") == "1":
        depth_interaction()
    benchmark("SetReferenceVisible", False)
    benchmark("SetOverlay", "Planar A/B: forced exhaustion | quad | front | maximum cells = 1")
    camera("front")
    property_value(component, "Maximum cells", 1)
    general.idle_wait_frames(120)
    capture("forced_exhaustion")
    property_value(component, "Maximum cells", manifest["max_cells"])
    property_value(component, "Width", 0.0)
    general.idle_wait_frames(10)
    invalid_status = silpom.SilPomPatchRequestBus(bus.Event, "GetStatus", patch)
    assert not silpom.SilPomPatchRequestBus(bus.Event, "IsReady", patch) and "Invalid" in invalid_status
    property_value(component, "Width", manifest["width_metres"])
    wait_until(lambda: silpom.SilPomPatchRequestBus(bus.Event, "IsReady", patch))
    result["diagnostics"] = {"invalid_config_status": invalid_status, "recovered": True,
                             "forced_exhaustion_capture": str(output / "forced_exhaustion.png")}
    result["passed"] = True  # Harness completed, NOT performance/quality acceptance.
except Exception:
    result["errors"].append(traceback.format_exc())
finally:
    benchmark("Release")
    if original_viewport:
        general.set_viewport_size(*original_viewport)
        general.set_viewport_expansion_policy(original_policy)
    for name, value in original_cvars.items():
        if value is not None and name != "ed_backgroundUpdatePeriod":
            general.run_console(name + " " + str(value))
    checkpoint()
    print("SILPOM_PLANAR_RESULT " + json.dumps(result))
    # Keep background ticks enabled until viewport/pipeline rebuilds drain.
    # Restoring a zero background period first stalls a hidden editor forever.
    general.idle_wait_frames(120)
    background = original_cvars.get("ed_backgroundUpdatePeriod")
    if background is not None:
        general.run_console("ed_backgroundUpdatePeriod " + str(background))
    general.exit_no_prompt()

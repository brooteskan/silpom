"""Capture 1080p budgeted previews of the saved fixture; leave Editor interactive.

Run with --runpython. Requires the saved SilPOM_Interactive_Test entity. Never
creates/deletes entities, changes their bindings/transforms, or saves the level.
Only this session's camera, viewport, shadow settings and preview cvar change.
"""
import hashlib
import json
from pathlib import Path
import struct
import sys
import traceback
import azlmbr.atom as atom
import azlmbr.bus as bus
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.paths as paths
import azlmbr.render as render
import azlmbr.silpom as silpom

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mesh_capture_analysis import validate_motion_albedo

output = Path(paths.projectroot) / "user/SilPOMMeshFullResolution"
output.mkdir(parents=True, exist_ok=True)
level = Path(paths.projectroot) / "Levels/DefaultLevel/DefaultLevel.prefab"
original_hash = hashlib.sha256(level.read_bytes()).hexdigest()
report = dict(passed=False, stage="startup", preview_only=True, saved_level=False, captures={})
original_background = None


def flush():
    (output / "result.json").write_text(json.dumps(report, indent=2))


def capture(name, hierarchy=None, attachment=None):
    filename = output / name
    if hierarchy is None:
        outcome = atom.FrameCaptureRequestBus(bus.Broadcast, "CaptureScreenshot", str(filename))
    else:
        outcome = atom.FrameCaptureRequestBus(bus.Broadcast, "CapturePassAttachment", str(filename), hierarchy, attachment, 1)
    assert outcome.IsSuccess(), "Cannot capture " + name
    done = {}
    handler = atom.FrameCaptureNotificationBusHandler(); handler.connect(outcome.GetValue())
    handler.add_callback("OnFrameCaptureFinished", lambda args: done.update(success=args[0] == atom.FrameCaptureResult_Success))
    try:
        for _ in range(180):
            if done: break
            general.idle_wait_frames(1)
    finally:
        handler.disconnect()
    assert done.get("success"), "Capture failed: " + name
    return filename


def inspect_hits(filename, budget):
    data = filename.read_bytes()
    batches = []
    for base in silpom.SilPomMeshRequestBus(bus.Event, "GetHitOffsets", mesh):
        count = struct.unpack_from("<I", data, base)[0]
        assert 0 < count <= 32
        views = []
        for view in range(count):
            header = base + 16 + view * 128
            width, height, near_z, error = struct.unpack_from("<4f", data, header + 64)
            left, top, w, h = struct.unpack_from("<4I", data, header + 80)
            records = struct.unpack_from("<I", data, header + 96)[0]
            stride, columns, rows, recorded_budget = struct.unpack_from("<4I", data, header + 112)
            assert error == 0 and recorded_budget == budget, (error, recorded_budget)
            assert stride >= 1 and columns == (w + stride - 1) // stride and rows == (h + stride - 1) // stride
            states = [0, 0, 0, 0]
            for sample in range(columns * rows):
                status = struct.unpack_from("<I", data, records + sample * 96 + 12)[0]
                assert 0 <= status <= 3
                states[status] += 1
            assert states[3] == 0, "Invalid GPU rays"
            views.append(dict(viewport=[int(width), int(height)], shadow=near_z == 0,
                rectangle=[left, top, w, h], stride=stride, grid=[columns, rows],
                rays=columns * rows, miss=states[0], hit=states[1], exhausted=states[2], invalid=states[3]))
        assert sum(v["rays"] for v in views) <= (budget or 65536), "Batch exceeds ray budget"
        batches.append(dict(offset=base, rays=sum(v["rays"] for v in views), views=views))
    assert batches and sum(v["hit"] for b in batches for v in b["views"] if not v["shadow"]) > 0
    return batches


def timestamps(name):
    done = {}
    handler = atom.ProfilingCaptureNotificationBusHandler(); handler.connect()
    handler.add_callback("OnCaptureQueryTimestampFinished", lambda args: done.update(success=bool(args[0])))
    try:
        accepted = atom.ProfilingCaptureRequestBus(bus.Broadcast, "CapturePassTimestamp", str(output / (name + "_gpu.json")))
        for _ in range(180):
            if not accepted or done: break
            general.idle_wait_frames(1)
        return dict(accepted=bool(accepted), **done)
    finally:
        handler.disconnect()


try:
    flush()
    general.idle_enable(True)
    original_background = general.get_cvar("ed_backgroundUpdatePeriod")
    general.run_console("ed_backgroundUpdatePeriod -1")
    general.run_console("r_silpomPreviewRayBudget 4096")
    general.run_console("r_meshInstancingEnabled false")
    if general.get_current_level_name() != "DefaultLevel":
        assert general.open_level("DefaultLevel")
    general.idle_wait_frames(30)
    general.run_console("r_multiSampleCount 1")
    general.run_console("r_displayInfo 0")
    general.idle_wait_frames(5)
    search = entity.SearchFilter(); search.names = ["SilPOM_Interactive_Test"]
    found = entity.SearchBus(bus.Broadcast, "SearchEntities", search)
    assert len(found) == 1, "Expected one saved SilPOM_Interactive_Test entity"
    mesh = found[0]
    editor.ToolsApplicationRequestBus(bus.Broadcast, "SetSelectedEntities", [])
    render.DirectionalLightRequestBus(bus.Broadcast, "SetShadowmapSize", 256)
    render.DirectionalLightRequestBus(bus.Broadcast, "SetCascadeCount", 1)
    render.DirectionalLightRequestBus(bus.Broadcast, "SetShadowFarClipDistance", 10.0)
    general.set_current_view_position(16.0, 11.0, 40.0)
    general.set_current_view_rotation(0.0, 0.0, 0.0)
    # Also exercise opt-out: native sampling must still work at the old size.
    general.set_viewport_expansion_policy("FixedSize")
    general.set_viewport_size(320, 240); general.update_viewport()
    general.run_console("r_silpomPreviewRayBudget 0")
    for _ in range(600):
        if silpom.SilPomMeshRequestBus(bus.Event, "IsReady", mesh): break
        general.idle_wait_frames(1)
    assert silpom.SilPomMeshRequestBus(bus.Event, "IsReady", mesh), silpom.SilPomMeshRequestBus(bus.Event, "GetStatus", mesh)
    general.idle_wait_frames(8)
    control = capture("native_control_hits.bin", ["SilPomResolve"], "SilPomComputedHits")
    report["native_control"] = inspect_hits(control, 0); flush()
    for budget in (1024, 4096):
        report["stage"] = "1080p budget " + str(budget); flush()
        general.run_console("r_silpomPreviewRayBudget " + str(budget))
        general.set_viewport_size(1920, 1080); general.update_viewport()
        general.idle_wait_frames(10)
        name = "preview_1080p_" + str(budget)
        hit_file = capture(name + "_hits.bin", ["SilPomResolve"], "SilPomComputedHits")
        albedo = capture(name + "_albedo.dds", ["MainPipeline_0", "OpaquePass", "Forward"], "AlbedoOutput")
        colors = validate_motion_albedo(albedo)
        assert colors["viewport"] == [1920, 1080] and colors["red"] > 0 and colors["blue"] > 0, colors
        rows = inspect_hits(hit_file, budget)
        assert all(v["viewport"] == [1920, 1080] for b in rows for v in b["views"] if not v["shadow"])
        png = capture(name + ".png")
        png_data = png.read_bytes()
        assert struct.unpack_from(">II", png_data, 16) == (1920, 1080)
        report["captures"][str(budget)] = dict(image=str(png), batches=rows, albedo=colors, timestamps=timestamps(name))
        flush()
    general.close_pane("Console")
    general.set_viewport_expansion_policy("AutoExpand"); general.update_viewport()
    general.idle_wait_frames(10)
    size = general.get_viewport_size()
    report.update(passed=True, stage="interactive full viewport", viewport=[int(size.x), int(size.y)],
        budget=4096, status=silpom.SilPomMeshRequestBus(bus.Event, "GetStatus", mesh),
        shadow_settings=dict(resolution=256, cascades=1, far_clip_metres=10),
        instructions="APPROXIMATE preview. Full-resolution output; block-sampled displacement/depth/shadows. Console: r_silpomPreviewRayBudget 1024 / 4096 / 16384. Zero restores strict sampling.")
except Exception:
    report.update(stage="error", error=traceback.format_exc())
finally:
    if original_background is not None:
        general.run_console("ed_backgroundUpdatePeriod " + str(original_background))
    report["saved_level_unchanged"] = hashlib.sha256(level.read_bytes()).hexdigest() == original_hash
    if not report["saved_level_unchanged"]: report["passed"] = False
    flush(); print(json.dumps(report, indent=2))
    # Leave this session open; never save or exit the user's level.

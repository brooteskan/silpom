"""Exercise the real dock, viewport event filter, GPU picks, undo and serialization.

Run in a dedicated Editor --runpython session. Never saves the user's level.
Synthetic Qt pen events test the input pipeline, not the physical tablet driver.
"""
import hashlib
import json
import math
import os
import runpy
from pathlib import Path
import time
import traceback

import azlmbr.atom as atom
import azlmbr.bus as bus
import azlmbr.components as components
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.math as azmath
import azlmbr.paths as paths
import azlmbr.physics as physics
import azlmbr.prefab as prefab
import azlmbr.silpom as silpom

from PySide6 import QtCore, QtGui, QtWidgets

project = Path(paths.projectroot)
output = project / "user/DecalPainterSmoke"
output.mkdir(parents=True, exist_ok=True)
level = project / "Levels/VueLiveLink/VueLiveLink.prefab"
result = {"passed": False, "stage": "starting"}


def stage(name):
    result["stage"] = name
    (output / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")


def wait(predicate, seconds=45):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        general.idle_wait_frames(1)
    raise TimeoutError(result["stage"])


def document():
    raw = silpom.DecalPaintRequestBus(bus.Event, "GetDocumentJson", canvas)
    if not raw:
        wait(lambda: bool(silpom.DecalPaintRequestBus(bus.Event, "GetDocumentJson", canvas)), 10)
        raw = silpom.DecalPaintRequestBus(bus.Event, "GetDocumentJson", canvas)
    return json.loads(raw)


def layers():
    d = document()
    return d.get("ClassData", d).get("Layers", [])


def strokes():
    return [s for layer_data in layers() for s in layer_data.get("Strokes", [])]


def ui(cls, name):
    found = panel.findChild(cls, name)
    assert found is not None, name
    return found


def pixel_pick(x, y):
    return silpom.DecalPainterBus(bus.Broadcast, "PickVisiblePixel", x, y)


def pointer(kind, x, y, pressure=None):
    local = QtCore.QPointF(x / viewport.devicePixelRatioF(), y / viewport.devicePixelRatioF())
    global_pos = viewport.mapToGlobal(local)
    down = kind in (QtCore.QEvent.Type.MouseButtonPress, QtCore.QEvent.Type.TabletPress)
    up = kind in (QtCore.QEvent.Type.MouseButtonRelease, QtCore.QEvent.Type.TabletRelease)
    button = QtCore.Qt.MouseButton.LeftButton if down or up else QtCore.Qt.MouseButton.NoButton
    buttons = QtCore.Qt.MouseButton.NoButton if up else QtCore.Qt.MouseButton.LeftButton
    if pressure is None:
        e = QtGui.QMouseEvent(kind, local, global_pos, button, buttons, QtCore.Qt.KeyboardModifier.NoModifier)
    else:
        e = QtGui.QTabletEvent(kind, pen, local, global_pos, pressure, 10.0, -5.0, 0.0, 0.0, 0.0,
                              QtCore.Qt.KeyboardModifier.NoModifier, button, buttons)
    QtWidgets.QApplication.sendEvent(viewport, e)


def capture(name):
    request = atom.FrameCaptureRequestBus(bus.Broadcast, "CaptureScreenshot", str(output / (name + ".png")))
    assert request.IsSuccess()
    finished = {}
    handler = atom.FrameCaptureNotificationBusHandler()
    handler.connect(request.GetValue())
    handler.add_callback("OnFrameCaptureFinished", lambda args: finished.update(success=args[0] == atom.FrameCaptureResult_Success))
    wait(lambda: bool(finished))
    handler.disconnect()
    assert finished["success"]


try:
    result["level_sha256"] = hashlib.sha256(level.read_bytes()).hexdigest()
    stage("opening level")
    general.idle_enable(True)
    if general.get_current_level_name().lower() != "vuelivelink":
        assert general.open_level(str(level))
    general.idle_wait_frames(150)
    query = entity.SearchFilter()
    query.names = ["Irish Fieldstone Tower (Open Shaft)"]
    tower = entity.SearchBus(bus.Broadcast, "SearchEntities", query)[0]
    wait(lambda: silpom.SilPomMeshRequestBus(bus.Event, "IsReady", tower), 90)
    types = editor.EditorComponentAPIBus(bus.Broadcast, "FindComponentTypeIdsByEntityType", ["SilPOM Mesh"], entity.EntityType().Game)
    component = editor.EditorComponentAPIBus(bus.Broadcast, "GetComponentOfType", tower, types[0]).GetValue()
    properties = editor.EditorComponentAPIBus(bus.Broadcast, "BuildComponentPropertyList", component)
    # Keep relief visible for this test regardless of the user's saved GPU budget.
    for label, value in [("Maximum relief distance (closer = cheaper)", 0.0),
                         ("Relief LOD pixel budget (higher = cheaper)", 0.25)]:
        prop = next(p for p in properties if p.endswith("|" + label))
        assert editor.EditorComponentAPIBus(bus.Broadcast, "SetComponentProperty", component, prop, value).IsSuccess()
    wait(lambda: silpom.SilPomMeshRequestBus(bus.Event, "IsReady", tower), 90)
    origin = components.TransformBus(bus.Event, "GetWorldTranslation", tower)
    result["tower_origin"] = [origin.x, origin.y, origin.z]
    # Close view of the curved exterior with the trained SILPOM wall profile.
    general.set_current_view_position(origin.x, origin.y - 8.0, origin.z + 8.0)
    general.set_current_view_rotation(0.0, 0.0, 0.0)
    general.update_viewport()
    stage("opening dock")
    canvas = silpom.DecalPainterBus(bus.Broadcast, "CreatePaintEntity")
    assert canvas.IsValid(), "No paint entity"
    app = QtWidgets.QApplication.instance()
    panel = next(w for w in app.allWidgets() if w.objectName() == "DecalPainter")
    viewport = QtWidgets.QWidget.find(silpom.DecalPainterBus(bus.Broadcast, "GetViewportWindowId"))
    assert viewport is not None
    ui(QtWidgets.QCheckBox, "EnablePainting").setChecked(True)
    general.idle_wait_frames(90)
    size = general.get_viewport_size()
    width, height = int(size.x), int(size.y)
    result["viewport"] = [width, height, viewport.metaObject().className()]
    result["panel_visible"] = panel.isVisible()
    result["viewport_visible"] = viewport.isVisible()
    result["dock_status_before_pick"] = ui(QtWidgets.QLabel, "DecalPaintStatus").text()
    result["viewport_widget_size"] = [viewport.width(), viewport.height(), viewport.devicePixelRatioF()]
    stage("GPU depth pick")
    result["picker_diagnostics"] = silpom.DecalPainterBus(bus.Broadcast, "GetPickerStatus")
    stage("GPU depth pick")
    capture("before_paint")
    x, y = width // 2, height // 2
    wait(lambda: pixel_pick(x, y).GetLength() > 1.0)
    picked = pixel_pick(x, y)
    result["visible_hit"] = [picked.x, picked.y, picked.z]
    result["dock_status"] = ui(QtWidgets.QLabel, "DecalPaintStatus").text()
    assert abs(picked.z - (origin.z + 8)) < 0.1, result["visible_hit"]
    # Record the difference from the base collider, which a physics-only brush
    # would incorrectly use. Require at least one relief hit in a nearby patch.
    eye = azmath.Vector3(origin.x, origin.y - 8.0, origin.z + 8.0)
    differences = []
    for py in range(y - 40, y + 41, 20):
        for px in range(x - 40, x + 41, 20):
            visible_hit = pixel_pick(px, py)
            if visible_hit.GetLength() < 1:
                continue
            ray = physics.RayCastRequest()
            ray.Start = eye
            delta = azmath.Vector3(visible_hit.x-eye.x, visible_hit.y-eye.y, visible_hit.z-eye.z)
            ray.Direction = delta.GetNormalized()
            ray.Distance = 20.0
            base_hit = physics.SimulatedBodyComponentRequestBus(bus.Event, "RayCast", tower, ray)
            if base_hit.EntityId.IsValid():
                differences.append(abs(delta.GetLength() - base_hit.Distance))
    result["max_visible_vs_collider_difference_m"] = max(differences, default=0)
    assert result["max_visible_vs_collider_difference_m"] > .001, differences
    stage("single stamp")
    pointer(QtCore.QEvent.Type.MouseButtonPress, x, y)
    pointer(QtCore.QEvent.Type.MouseButtonRelease, x, y)
    general.idle_wait_frames(12)
    result["first_document"] = document()
    assert len(strokes()) == 1 and len(strokes()[0]["Points"]) == 1, result["first_document"]
    assert silpom.DecalPaintRequestBus(bus.Event, "GetDabCount", canvas) == 1
    # Real editor undo re-instantiates the component, testing bus/resource cleanup.
    stage("stroke undo redo")
    general.undo()
    general.idle_wait_frames(12)
    assert len(strokes()) == 0
    general.redo()
    general.idle_wait_frames(12)
    assert len(strokes()) == 1
    assert len(layers()) == 1, "Undo/redo inserted a default layer"
    stage("continuous stroke")
    ui(QtWidgets.QComboBox, "PaintMode").setCurrentIndex(1)
    wait(lambda: pixel_pick(x-70, y+30).GetLength() > 1.0)
    pointer(QtCore.QEvent.Type.MouseButtonPress, x - 70, y + 30)
    for dx in range(-60, 71, 10):
        pointer(QtCore.QEvent.Type.MouseMove, x + dx, y + 30)
    pointer(QtCore.QEvent.Type.MouseButtonRelease, x + 70, y + 30)
    general.idle_wait_frames(20)
    result["continuous_document"] = document()
    assert len(strokes()) == 2 and len(strokes()[1]["Points"]) > 10
    count = silpom.DecalPaintRequestBus(bus.Event, "GetDabCount", canvas)
    assert count > 2, count
    result["continuous_dabs"] = count
    general.undo()
    general.idle_wait_frames(12)
    assert len(strokes()) == 1, "Continuous stroke needs exactly one undo"
    general.redo()
    general.idle_wait_frames(12)
    stage("layers and lock")
    ui(QtWidgets.QPushButton, "AddLayer").click()
    assert len(layers()) == 2
    ui(QtWidgets.QCheckBox, "PaintLayerLocked").click()
    pointer(QtCore.QEvent.Type.MouseButtonPress, x, y - 30)
    pointer(QtCore.QEvent.Type.MouseButtonRelease, x, y - 30)
    assert len(strokes()) == 2, "Locked layer accepted paint"
    ui(QtWidgets.QCheckBox, "PaintLayerLocked").click()
    stage("tablet pressure events")
    wait(lambda: pixel_pick(x-55, y-45).GetLength() > 1.0)
    pen = QtGui.QPointingDevice("Synthetic test pen", 9251, QtGui.QInputDevice.DeviceType.Stylus,
                               QtGui.QPointingDevice.PointerType.Pen,
                               QtGui.QInputDevice.Capability.Position | QtGui.QInputDevice.Capability.Pressure,
                               1, 2)
    pointer(QtCore.QEvent.Type.TabletPress, x - 55, y - 45, 0.25)
    pointer(QtCore.QEvent.Type.TabletMove, x, y - 45, 0.5)
    pointer(QtCore.QEvent.Type.TabletMove, x + 55, y - 45, 0.8)
    pointer(QtCore.QEvent.Type.TabletRelease, x + 55, y - 45, 0.0)
    # Driver-emulated mouse events arrive immediately after the handled pen event.
    pointer(QtCore.QEvent.Type.MouseButtonPress, x, y)
    pointer(QtCore.QEvent.Type.MouseButtonRelease, x, y)
    general.idle_wait_frames(20)
    assert len(strokes()) == 3
    pressures = [p.get("Pressure", 1) for p in strokes()[2]["Points"]]
    assert min(pressures) < 0.3 and max(pressures) > 0.75, [min(pressures), max(pressures)]
    result["pen_pressure_range"] = [min(pressures), max(pressures)]
    result["pen_status"] = ui(QtWidgets.QLabel, "TabletStatus").text()
    # A mouse event immediately following a pen event must not create a duplicate.
    assert len(strokes()) == 3
    stage("edit stroke and point")
    ui(QtWidgets.QDoubleSpinBox, "BrushDiameter").setValue(.45)
    ui(QtWidgets.QDoubleSpinBox, "BrushNormalOpacity").setValue(.2)
    ui(QtWidgets.QPushButton, "ApplyStroke").click()
    assert math.isclose(strokes()[2]["Diameter"], .45, rel_tol=1e-5)
    assert math.isclose(strokes()[2]["NormalOpacity"], .2, rel_tol=1e-5)
    ui(QtWidgets.QComboBox, "PaintMode").setCurrentIndex(2)
    ui(QtWidgets.QListWidget, "StrokePoints").setCurrentRow(0)
    previous = strokes()[2]["Points"][0]["Position"]
    general.idle_wait_frames(30)
    pointer(QtCore.QEvent.Type.MouseButtonPress, x - 40, y - 70)
    pointer(QtCore.QEvent.Type.MouseButtonRelease, x - 40, y - 70)
    general.idle_wait_frames(12)
    assert strokes()[2]["Points"][0]["Position"] != previous
    general.undo()
    general.idle_wait_frames(12)
    assert strokes()[2]["Points"][0]["Position"] == previous
    stage("layer visibility")
    tree = ui(QtWidgets.QTreeWidget, "PaintLayers")
    tree.setCurrentItem(tree.topLevelItem(1))
    visible = ui(QtWidgets.QCheckBox, "PaintItemVisible")
    visible.click()
    general.idle_wait_frames(10)
    assert silpom.DecalPaintRequestBus(bus.Event, "GetDabCount", canvas) == count
    visible.click()
    general.idle_wait_frames(10)
    stage("layer ordering and undo")
    ui(QtWidgets.QPushButton, "LowerLayer").click()
    assert layers()[0].get("Name") == "Layer 2"
    general.undo()
    general.idle_wait_frames(12)
    assert layers()[1].get("Name") == "Layer 2"
    stage("cancel gesture")
    ui(QtWidgets.QComboBox, "PaintMode").setCurrentIndex(1)
    wait(lambda: pixel_pick(x, y).GetLength() > 1.0)
    pointer(QtCore.QEvent.Type.MouseButtonPress, x, y)
    pointer(QtCore.QEvent.Type.MouseMove, x+20, y)
    cancel = QtGui.QKeyEvent(QtCore.QEvent.Type.KeyPress, QtCore.Qt.Key.Key_Escape, QtCore.Qt.KeyboardModifier.NoModifier)
    QtWidgets.QApplication.sendEvent(viewport, cancel)
    general.idle_wait_frames(10)
    assert len(strokes()) == 3 and not ui(QtWidgets.QCheckBox, "EnablePainting").isChecked()
    stage("prefab serialization")
    result["document"] = document()
    error = silpom.DecalPainterBus(bus.Broadcast, "SavePaintPrefab", canvas, str(output / "paint.prefab"))
    assert not error, error
    encoded = (output / "paint.prefab").read_text(encoding="utf-8")
    result["serialized_bytes"] = len(encoded)
    assert "Pressure" in encoded and "Layers" in encoded and "Points" in encoded
    assert "soft_round.azmaterial" in encoded
    stage("visual capture")
    ui(QtWidgets.QCheckBox, "EnablePainting").setChecked(False)
    editor.ToolsApplicationRequestBus(bus.Broadcast, "SetSelectedEntities", [])
    general.idle_wait_frames(20)
    capture("painted_tower")
    # Saving the test prefab must not save the current level or source tower.
    assert hashlib.sha256(level.read_bytes()).hexdigest() == result["level_sha256"]
    result["passed"] = True
    stage("complete")
except Exception:
    result["error"] = traceback.format_exc()
    if "panel" in globals():
        result["failure_dock_status"] = ui(QtWidgets.QLabel, "DecalPaintStatus").text()
        result["picker_diagnostics"] = silpom.DecalPainterBus(bus.Broadcast, "GetPickerStatus")
        result["failure_mode"] = ui(QtWidgets.QComboBox, "PaintMode").currentIndex()
        result["failure_armed"] = ui(QtWidgets.QCheckBox, "EnablePainting").isChecked()
        result["failure_material"] = ui(QtWidgets.QLineEdit, "DecalMaterial").text()
        raw = silpom.DecalPaintRequestBus(bus.Event, "GetDocumentJson", canvas)
        result["failure_document"] = json.loads(raw) if raw else None
    stage("failed: " + result["stage"])
finally:
    print(json.dumps(result, indent=2))
    if result["passed"] and os.environ.get("DECAL_PAINTER_CHAIN_EVENT_LOOP"):
        QtCore.QTimer.singleShot(500, lambda path=str(Path(__file__).with_name("decal_painter_event_loop_smoke.py")): runpy.run_path(path, run_name="__main__"))
    else:
        general.exit_no_prompt()

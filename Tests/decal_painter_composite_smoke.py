"""Rolling 100-dab retention, native undo, >4096 projections and GPU equivalence.

Run in a dedicated Editor --runpython session. QTimer returns control to the
normal event loop between checks. Never saves VueLiveLink; exits automatically.
"""
import copy
import hashlib
import json
import time
import traceback
from pathlib import Path
import azlmbr.atom as atom
import azlmbr.bus as bus
import azlmbr.components as components
import azlmbr.entity as entity
import azlmbr.editor as editor
import azlmbr.legacy.general as general
import azlmbr.paths as paths
import azlmbr.silpom as silpom
from PySide6 import QtCore, QtGui, QtWidgets

project = Path(paths.projectroot)
output = project / "user/DecalPainterCompositeSmoke"
output.mkdir(parents=True, exist_ok=True)
level = project / "Levels/VueLiveLink/VueLiveLink.prefab"
result = {"passed": False, "stage": "startup", "level_sha256": hashlib.sha256(level.read_bytes()).hexdigest()}
original_background = None
original_display_info = None


def finish():
    # FixedSize is transient per Editor window. Reparenting/resizing its native
    # render widget during shutdown can invalidate passes still being torn down.
    if original_background is not None:
        general.run_console("ed_backgroundUpdatePeriod " + str(original_background))
    if original_display_info is not None:
        general.run_console("r_displayInfo " + str(original_display_info))
    general.exit_no_prompt()


def flush():
    (output / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")


def data(canvas):
    raw = json.loads(silpom.DecalPaintRequestBus(bus.Event, "GetDocumentJson", canvas))
    return raw.get("ClassData", raw)


def put(canvas, d):
    document = {"Type": "JsonSerialization", "Version": 1, "ClassName": "PaintConfig", "ClassData": d}
    error = silpom.DecalPainterBus(bus.Broadcast, "SetPaintDocument", canvas, json.dumps(document))
    assert not error, error


def count(canvas):
    d = data(canvas)
    live = [p for l in d.get("Layers", []) for s in l.get("Strokes", []) for p in s.get("Points", [])]
    fixed = [p for l in d.get("Layers", []) for p in l.get("Composite", [])]
    return live, fixed


def mouse(viewport, event_type, x, y):
    local = QtCore.QPointF(x / viewport.devicePixelRatioF(), y / viewport.devicePixelRatioF())
    up = event_type == QtCore.QEvent.Type.MouseButtonRelease
    move = event_type == QtCore.QEvent.Type.MouseMove
    event = QtGui.QMouseEvent(event_type, local, viewport.mapToGlobal(local),
                             QtCore.Qt.MouseButton.NoButton if move else QtCore.Qt.MouseButton.LeftButton,
                             QtCore.Qt.MouseButton.NoButton if up else QtCore.Qt.MouseButton.LeftButton,
                             QtCore.Qt.KeyboardModifier.NoModifier)
    QtWidgets.QApplication.sendEvent(viewport, event)


def click(viewport, x, y):
    mouse(viewport, QtCore.QEvent.Type.MouseButtonPress, x, y)
    mouse(viewport, QtCore.QEvent.Type.MouseButtonRelease, x, y)


class Check(QtCore.QObject):
    def __init__(self):
        super().__init__(QtWidgets.QApplication.instance())
        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(250)
        self.timer.timeout.connect(self.advance)
        self.started = time.monotonic()
        self.step = 0
        self.visual_case = "mixed"
        self.run_id = str(time.time_ns())
        self.pulses = 0
        self.capture_handler = None
        self.capture_done = {}
        self.canvas = silpom.DecalPainterBus(bus.Broadcast, "CreatePaintEntity")
        self.panel = next(w for w in QtWidgets.QApplication.allWidgets() if w.objectName() == "DecalPainter")
        self.viewport = QtWidgets.QWidget.find(silpom.DecalPainterBus(bus.Broadcast, "GetViewportWindowId"))
        self.viewport.window().setWindowTitle("Paint composite verification — closes automatically")
        self.arm = self.panel.findChild(QtWidgets.QCheckBox, "EnablePainting")
        self.mode = self.panel.findChild(QtWidgets.QComboBox, "PaintMode")
        self.mode.setCurrentIndex(0)
        self.panel.findChild(QtWidgets.QDoubleSpinBox, "BrushDiameter").setValue(.3)
        self.panel.findChild(QtWidgets.QDoubleSpinBox, "BrushOpacity").setValue(.6)
        editor.ToolsApplicationRequestBus(bus.Broadcast, "SetSelectedEntities", [])
        self.arm.setChecked(True)
        self.timer.start()

    def capture(self, name):
        self.capture_done = {}
        request = atom.FrameCaptureRequestBus(bus.Broadcast, "CaptureScreenshot", str(output / (name + ".png")))
        assert request.IsSuccess()
        self.capture_handler = atom.FrameCaptureNotificationBusHandler()
        self.capture_handler.connect(request.GetValue())
        self.capture_handler.add_callback("OnFrameCaptureFinished", lambda args: self.capture_done.update(success=args[0] == atom.FrameCaptureResult_Success))

    def captured(self):
        if not self.capture_done:
            return False
        assert self.capture_done["success"]
        self.capture_handler.disconnect()
        return True

    def fixture(self, count, mixed=False):
        base = copy.deepcopy(self.prototype)
        base["Layers"][0]["Id"] = 1
        base["Layers"][0]["Order"] = []
        base["Layers"][0].pop("Composite", None)
        base["NextId"] = count * 2 + 5000
        base["FrozenThrough"] = 0
        strokes = []
        center = self.point["Position"]
        for start in range(0, count, 25):
            stroke = copy.deepcopy(self.style)
            # Run identities and dab ages share the document's ID allocator.
            stroke.update(Id=count + 2000 + start, ExplicitDabs=True, Name="Regression stroke", Points=[])
            stroke["PressureOpacity"] = True
            if mixed:
                stroke["BlendMode"] = (start // 25) % 5
                stroke["NormalOpacity"] = .2 if start == 25 else 0
                stroke["Color"] = [.2 + (start % 3) * .2, .15, .7]
            for i in range(start, min(start + 25, count)):
                point = copy.deepcopy(self.point)
                point["Position"] = [center[0] + ((i % 17) - 8) * .035, center[1], center[2] + ((i // 17) % 13 - 6) * .025]
                point["Sequence"] = 100 + i
                point["Pressure"] = .25 + .75 * (i % 9) / 8
                stroke["Points"].append(point)
            strokes.append(stroke)
            base["Layers"][0]["Order"].append(stroke["Id"])
        base["Layers"][0]["Strokes"] = strokes
        return base

    def new_canvas(self):
        canvas = silpom.DecalPainterBus(bus.Broadcast, "CreatePaintEntity")
        # CreateNewEntity chooses its transform from the current scene/selection.
        # Fixture placements are local to the original canvas, so copy its pose.
        components.TransformBus(bus.Event, "SetWorldTM", canvas, self.paint_world)
        editor.ToolsApplicationRequestBus(bus.Broadcast, "SetSelectedEntities", [])
        return canvas

    def advance(self):
        try:
            self.pulses += 1
            assert time.monotonic() - self.started < 180, "Composite verification timed out"
            size = general.get_viewport_size()
            x, y = int(size.x) // 2, int(size.y) // 2
            if self.step == 0:
                result["stage"] = "waiting for depth"
                result["picker"] = silpom.DecalPainterBus(bus.Broadcast, "GetPickerStatus")
                if silpom.DecalPainterBus(bus.Broadcast, "PickVisiblePixel", x, y).GetLength() < 1:
                    flush()
                    return
                self.capture("baseline")
                self.step = -1
            elif self.step == -1:
                if not self.captured():
                    return
                self.viewport.window().activateWindow()
                self.viewport.setFocus(QtCore.Qt.FocusReason.MouseFocusReason)
                click(self.viewport, x, y)
                self.step = 1
            elif self.step == 1:
                result["stage"] = "waiting for first painted batch"
                result["paint_status"] = silpom.DecalPaintRequestBus(bus.Event, "GetPaintStatus", self.canvas)
                result["first_live_count"] = len(count(self.canvas)[0])
                result["picker_now"] = silpom.DecalPainterBus(bus.Broadcast, "GetPickerStatus")
                result["dock_status"] = self.panel.findChild(QtWidgets.QLabel, "DecalPaintStatus").text()
                result["widget_size"] = [self.viewport.width(), self.viewport.height(), self.viewport.devicePixelRatioF()]
                result["material"] = self.panel.findChild(QtWidgets.QLineEdit, "DecalMaterial").text()
                result["initial_document"] = data(self.canvas)
                flush()
                if silpom.DecalPaintRequestBus(bus.Event, "GetRenderHandleCount", self.canvas) != 1:
                    if not result["first_live_count"] and self.pulses % 8 == 0:
                        click(self.viewport, x, y)
                    return
                self.prototype = data(self.canvas)
                self.paint_world = components.TransformBus(bus.Event, "GetWorldTM", self.canvas)
                self.style = copy.deepcopy(self.prototype["Layers"][0]["Strokes"][0])
                self.point = copy.deepcopy(self.style["Points"][0])
                put(self.canvas, self.fixture(5000))
                self.step = 2
            elif self.step == 2:
                result["stage"] = "5000 dabs and exact retention"
                live, fixed = count(self.canvas)
                assert len(live) == 100 and len(fixed) == 4900, (len(live), len(fixed))
                assert min(p["Sequence"] for p in live) == 5000
                assert abs(fixed[0]["Diameter"] - .075) < 1e-5
                assert abs(fixed[0]["Opacity"] - .15) < 1e-5
                assert silpom.DecalPaintRequestBus(bus.Event, "GetRenderHandleCount", self.canvas) == 1
                result["many_dabs"] = {"live": len(live), "composite": len(fixed), "render_batches": 1}
                editor.ToolsApplicationRequestBus(bus.Broadcast, "SetSelectedEntities", [])
                if not hasattr(self, "many_wait"):
                    self.many_wait = time.monotonic() + 2
                    return
                if time.monotonic() < self.many_wait:
                    return
                self.capture("5000-dabs")
                self.step = 21
            elif self.step == 21:
                if not self.captured():
                    return
                click(self.viewport, x + 20, y)
                self.step = 3
            elif self.step == 3:
                result["stage"] = "native undo across composite frontier"
                live, fixed = count(self.canvas)
                result["frontier_counts"] = [len(live), len(fixed)]
                result["frontier_picker"] = silpom.DecalPainterBus(bus.Broadcast, "GetPickerStatus")
                if (len(live), len(fixed)) == (100, 4900):
                    # Importing a large fixture propagates through prefab and
                    # viewport state. Retry only if no new dab was emitted.
                    if self.pulses % 8 == 0:
                        click(self.viewport, x + 20, y)
                    flush()
                    return
                assert (len(live), len(fixed)) == (100, 4901), (len(live), len(fixed))
                general.undo()
                self.step = 4
            elif self.step == 4:
                live, fixed = count(self.canvas)
                assert (len(live), len(fixed)) == (99, 4901), (len(live), len(fixed))
                general.redo()
                self.step = 5
            elif self.step == 5:
                live, fixed = count(self.canvas)
                result["redo_counts"] = [len(live), len(fixed)]
                assert (len(live), len(fixed)) == (100, 4901), (len(live), len(fixed))
                result["native_undo_redo"] = True
                # A saved prefab and a fresh controller must retain the fixed paint.
                error = silpom.DecalPainterBus(bus.Broadcast, "SavePaintPrefab", self.canvas, str(output / "composite.prefab"))
                assert not error, error
                encoded = (output / "composite.prefab").read_text(encoding="utf-8")
                assert "Composite" in encoded and "FrozenThrough" in encoded
                snapshot = data(self.canvas)
                snapshot["Layers"][0]["Visible"] = False
                put(self.canvas, snapshot)
                self.roundtrip = self.new_canvas()
                snapshot["Layers"][0]["Visible"] = True
                put(self.roundtrip, snapshot)
                self.step = 6
            elif self.step == 6:
                live, fixed = count(self.roundtrip)
                assert (len(live), len(fixed)) == (100, 4901)
                result["roundtrip"] = True
                empty = data(self.roundtrip)
                empty["Layers"] = []
                put(self.roundtrip, empty)
                self.step = 60
            elif self.step == 60:
                assert tuple(map(len, count(self.roundtrip))) == (0, 0)
                general.undo()
                self.step = 61
            elif self.step == 61:
                assert tuple(map(len, count(self.roundtrip))) == (100, 4901)
                general.redo()
                self.step = 62
            elif self.step == 62:
                assert tuple(map(len, count(self.roundtrip))) == (0, 0)
                general.undo()
                self.step = 63
            elif self.step == 63:
                assert tuple(map(len, count(self.roundtrip))) == (100, 4901)
                result["layer_delete_undo_redo"] = True
                snapshot = data(self.roundtrip)
                snapshot["Layers"][0]["Visible"] = False
                put(self.roundtrip, snapshot)
                self.visual = self.new_canvas()
                put(self.visual, self.fixture(150, mixed=True))
                general.run_console("r_silpomPaintCompositeReference false")
                self.wait_until = time.monotonic() + 2
                self.step = 7
            elif self.step == 7:
                if time.monotonic() < self.wait_until:
                    return
                result["stage"] = "capturing spatial composite"
                self.capture(self.visual_case + "-spatial")
                atom.ProfilingCaptureRequestBus(bus.Broadcast, "CapturePassTimestamp", str(output / (self.run_id + "-" + self.visual_case + "-spatial-timing.json")))
                self.step = 8
            elif self.step == 8:
                if not self.captured():
                    return
                general.run_console("r_silpomPaintCompositeReference true")
                self.wait_until = time.monotonic() + 2
                self.step = 9
            elif self.step == 9:
                if time.monotonic() < self.wait_until:
                    return
                self.capture(self.visual_case + "-reference")
                atom.ProfilingCaptureRequestBus(bus.Broadcast, "CapturePassTimestamp", str(output / (self.run_id + "-" + self.visual_case + "-reference-timing.json")))
                self.step = 10
            elif self.step == 10:
                if not self.captured():
                    return
                a, b = QtGui.QImage(str(output / (self.visual_case + "-spatial.png"))), QtGui.QImage(str(output / (self.visual_case + "-reference.png")))
                errors = []
                for py in range(y - 70, y + 71, 2):
                    for px in range(x - 70, x + 71, 2):
                        ca, cb = a.pixelColor(px, py), b.pixelColor(px, py)
                        errors.extend([abs(ca.red() - cb.red()), abs(ca.green() - cb.green()), abs(ca.blue() - cb.blue())])
                result["reference_mean_rgb_error"] = sum(errors) / len(errors)
                result["reference_max_rgb_error"] = max(errors)
                assert result["reference_mean_rgb_error"] < 2, result["reference_mean_rgb_error"]
                result.setdefault("reference_errors", {})[self.visual_case] = {"mean": sum(errors) / len(errors), "maximum": max(errors)}
                general.run_console("r_silpomPaintCompositeReference false")
                snapshot = data(self.visual)
                snapshot["Layers"][0]["Visible"] = False
                put(self.visual, snapshot)
                if self.visual_case == "mixed":
                    self.visual_case = "alpha"
                    self.visual = self.new_canvas()
                    put(self.visual, self.fixture(750))
                    self.wait_until = time.monotonic() + 2
                    self.step = 7
                    return
                baseline = QtGui.QImage(str(output / "baseline.png"))
                changes = []
                for py in range(y - 60, y + 61, 3):
                    for px in range(x - 60, x + 61, 3):
                        ca, cb = a.pixelColor(px, py), baseline.pixelColor(px, py)
                        changes.extend([abs(ca.red() - cb.red()), abs(ca.green() - cb.green()), abs(ca.blue() - cb.blue())])
                result["visible_paint_mean_rgb_change"] = sum(changes) / len(changes)
                assert result["visible_paint_mean_rgb_change"] > 3, "Paint was not visible in the rendered comparison"
                self.long = self.new_canvas()
                self.mode.setCurrentIndex(1)
                self.panel.findChild(QtWidgets.QDoubleSpinBox, "BrushDiameter").setValue(.03)
                self.panel.findChild(QtWidgets.QDoubleSpinBox, "BrushSpacing").setValue(.05)
                mouse(self.viewport, QtCore.QEvent.Type.MouseButtonPress, x - 220, y)
                for px in range(x - 219, x + 221):
                    mouse(self.viewport, QtCore.QEvent.Type.MouseMove, px, y)
                mouse(self.viewport, QtCore.QEvent.Type.MouseButtonRelease, x + 220, y)
                self.step = 11
            elif self.step == 11:
                result["stage"] = "long gesture splits inside stroke"
                live, fixed = count(self.long)
                assert len(live) == 100 and len(fixed) > 0, (len(live), len(fixed))
                result["long_gesture"] = {"live": len(live), "fixed": len(fixed)}
                result["timing_files"] = [str(p) for p in output.glob(self.run_id + "*-timing.json")]
                assert hashlib.sha256(level.read_bytes()).hexdigest() == result["level_sha256"]
                assert "Trace::Assert" not in (project / "user/log/Editor.log").read_text(encoding="utf-8", errors="replace")
                result["editor_assertions"] = 0
                result.update(passed=True, stage="complete", timer_pulses=self.pulses)
                self.timer.stop()
                flush()
                QtCore.QTimer.singleShot(300, finish)
            flush()
        except Exception:
            result["error"] = traceback.format_exc()
            self.timer.stop()
            general.run_console("r_silpomPaintCompositeReference false")
            flush()
            QtCore.QTimer.singleShot(100, finish)


try:
    flush()
    general.idle_enable(True)
    original_background = general.get_cvar("ed_backgroundUpdatePeriod")
    general.run_console("ed_backgroundUpdatePeriod -1")
    original_display_info = general.get_cvar("r_displayInfo")
    general.run_console("r_displayInfo 0")
    if general.get_current_level_name().lower() != "vuelivelink":
        assert general.open_level(str(level))
    general.idle_wait_frames(150)
    general.set_viewport_expansion_policy("FixedSize")
    general.set_viewport_size(640, 360)
    query = entity.SearchFilter()
    query.names = ["Irish Fieldstone Tower (Open Shaft)"]
    tower = entity.SearchBus(bus.Broadcast, "SearchEntities", query)[0]
    origin = components.TransformBus(bus.Event, "GetWorldTranslation", tower)
    # Within the level's one-metre relief cap, so this exercises the displaced
    # SILPOM surface rather than only its distant shell.
    general.set_current_view_position(origin.x, origin.y - 4.8, origin.z + 8.0)
    general.set_current_view_rotation(0.0, 0.0, 0.0)
    general.update_viewport()
    composite_check = Check()
except Exception:
    result["error"] = traceback.format_exc()
    flush()
    finish()

"""Check focus, paint, selection and rendering in the normal Qt event loop.

The startup script returns before gestures run. QTimer drives short checks and
automatically closes the dedicated Editor without saving the level.
"""
import hashlib
import json
from pathlib import Path
import time
import traceback
import azlmbr.atom as atom
import azlmbr.bus as bus
import azlmbr.components as components
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.paths as paths
import azlmbr.silpom as silpom
from PySide6 import QtCore, QtGui, QtWidgets

project = Path(paths.projectroot)
output = project / "user/DecalPainterEventLoopSmoke"
output.mkdir(parents=True, exist_ok=True)
level = project / "Levels/VueLiveLink/VueLiveLink.prefab"
result = {"passed": False, "stage": "startup", "level_sha256": hashlib.sha256(level.read_bytes()).hexdigest()}


def flush():
    (output / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")


def click(viewport, x, y):
    local = QtCore.QPointF(x / viewport.devicePixelRatioF(), y / viewport.devicePixelRatioF())
    global_pos = viewport.mapToGlobal(local)
    for event_type, buttons in [(QtCore.QEvent.Type.MouseButtonPress, QtCore.Qt.MouseButton.LeftButton),
                                (QtCore.QEvent.Type.MouseButtonRelease, QtCore.Qt.MouseButton.NoButton)]:
        e = QtGui.QMouseEvent(event_type, local, global_pos, QtCore.Qt.MouseButton.LeftButton,
                             buttons, QtCore.Qt.KeyboardModifier.NoModifier)
        QtWidgets.QApplication.sendEvent(viewport, e)


class Check(QtCore.QObject):
    def __init__(self):
        super().__init__(QtWidgets.QApplication.instance())
        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(250)
        self.timer.timeout.connect(self.advance)
        self.started = time.monotonic()
        self.step = 0
        self.pulses = 0
        self.capture_handler = None
        self.capture_done = {}
        self.canvas = silpom.DecalPainterBus(bus.Broadcast, "CreatePaintEntity")
        assert self.canvas.IsValid()
        self.panel = next(w for w in QtWidgets.QApplication.allWidgets() if w.objectName() == "DecalPainter")
        self.viewport = QtWidgets.QWidget.find(silpom.DecalPainterBus(bus.Broadcast, "GetViewportWindowId"))
        self.viewport.window().setWindowTitle("Decal Painter verification — closes automatically")
        self.arm = self.panel.findChild(QtWidgets.QCheckBox, "EnablePainting")
        self.panel.findChild(QtWidgets.QComboBox, "PaintMode").setCurrentIndex(0)
        self.arm.setChecked(True)
        self.timer.start()

    def advance(self):
        try:
            self.pulses += 1
            assert time.monotonic()-self.started < 30, "Event-loop check timed out"
            size = general.get_viewport_size()
            x, y = int(size.x)//2, int(size.y)//2
            if self.step == 0:
                result["stage"] = "waiting for depth in normal event loop"
                hit = silpom.DecalPainterBus(bus.Broadcast, "PickVisiblePixel", x, y)
                if hit.GetLength() < 1:
                    flush()
                    return
                self.viewport.window().activateWindow()
                self.viewport.setFocus(QtCore.Qt.FocusReason.MouseFocusReason)
                click(self.viewport, x, y)
                self.step = 1
            elif self.step == 1:
                result["stage"] = "checking stamp and focus"
                assert silpom.DecalPaintRequestBus(bus.Event, "GetDabCount", self.canvas) == 1
                self.arm.setChecked(False)
                # Exercise the ordinary viewport click path too (including its collider query).
                click(self.viewport, x+30, y)
                self.step = 2
            elif self.step == 2:
                result["stage"] = "capturing rendered frame"
                request = atom.FrameCaptureRequestBus(bus.Broadcast, "CaptureScreenshot", str(output / "normal_event_loop.png"))
                assert request.IsSuccess()
                self.capture_handler = atom.FrameCaptureNotificationBusHandler()
                self.capture_handler.connect(request.GetValue())
                self.capture_handler.add_callback("OnFrameCaptureFinished", lambda args: self.capture_done.update(success=args[0] == atom.FrameCaptureResult_Success))
                self.step = 3
            elif self.step == 3:
                if not self.capture_done:
                    return
                assert self.capture_done["success"]
                image = QtGui.QImage(str(output / "normal_event_loop.png"))
                assert not image.isNull()
                samples = [image.pixelColor(image.width()*px//10,image.height()*py//10)
                           for px in range(2,9) for py in range(2,9)]
                result["mean_rgb_sum"] = sum(c.red()+c.green()+c.blue() for c in samples)/len(samples)
                assert result["mean_rgb_sum"] > 15, "Render stayed black after focus/click"
                result["event_loop_timer_pulses"] = self.pulses
                result["level_unchanged"] = hashlib.sha256(level.read_bytes()).hexdigest() == result["level_sha256"]
                assert result["level_unchanged"]
                result["passed"] = True
                result["stage"] = "complete"
                self.timer.stop()
                self.capture_handler.disconnect()
                flush()
                QtCore.QTimer.singleShot(500, general.exit_no_prompt)
            flush()
        except Exception:
            result["error"] = traceback.format_exc()
            self.timer.stop()
            flush()
            QtCore.QTimer.singleShot(100, general.exit_no_prompt)


try:
    flush()
    general.idle_enable(True)
    if general.get_current_level_name().lower() != "vuelivelink":
        assert general.open_level(str(level))
    general.idle_wait_frames(150)
    query = entity.SearchFilter()
    query.names = ["Irish Fieldstone Tower (Open Shaft)"]
    tower = entity.SearchBus(bus.Broadcast, "SearchEntities", query)[0]
    origin = components.TransformBus(bus.Event, "GetWorldTranslation", tower)
    general.set_current_view_position(origin.x,origin.y-8.0,origin.z+8.0)
    general.set_current_view_rotation(0.0,0.0,0.0)
    general.update_viewport()
    event_loop_check = Check()
    # Return to the Editor; no Python polling loop remains active.
except Exception:
    result["error"] = traceback.format_exc()
    flush()
    general.exit_no_prompt()

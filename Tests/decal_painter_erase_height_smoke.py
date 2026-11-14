"""Rendered paint/erase, independent height selections and Composite persistence.

Run with a dedicated Editor --runpython. Reuses the composite test's setup and
capture helpers; never saves VueLiveLink and closes automatically.
"""
from pathlib import Path
import azlmbr.paths as paths

base_path = Path(paths.projectroot) / "Gems/silpom/Tests/decal_painter_composite_smoke.py"
source = base_path.read_text(encoding="utf-8")
helpers, startup = source.split("\ntry:\n    flush()", 1)
exec(helpers.replace('"user/DecalPainterCompositeSmoke"', '"user/DecalPainterEraseHeightSmoke"'), globals())


class EraseCheck(Check):
    def __init__(self):
        super().__init__()
        self.panel.findChild(QtWidgets.QLineEdit, "DecalMaterial").setText("materials/decalpaint/hard_round.azmaterial")
        self.operation = self.panel.findChild(QtWidgets.QComboBox, "BrushOperation")
        self.case_index = 0
        self.current_canvas = None

    def bands(self, operation, mask):
        for i, name in enumerate(["Low", "Medium", "High"]):
            self.panel.findChild(QtWidgets.QCheckBox, operation + "Height" + name).setChecked(bool(mask & (1 << i)))

    def tablet(self, kind, x, y, pressure):
        local = QtCore.QPointF(x / self.viewport.devicePixelRatioF(), y / self.viewport.devicePixelRatioF())
        up = kind == QtCore.QEvent.Type.TabletRelease
        button = QtCore.Qt.MouseButton.NoButton if kind == QtCore.QEvent.Type.TabletMove else QtCore.Qt.MouseButton.LeftButton
        event = QtGui.QTabletEvent(kind, self.pen, local, self.viewport.mapToGlobal(local), pressure,
                                  0.0, 0.0, 0.0, 0.0, 0.0, QtCore.Qt.KeyboardModifier.NoModifier,
                                  button, QtCore.Qt.MouseButton.NoButton if up else QtCore.Qt.MouseButton.LeftButton)
        QtWidgets.QApplication.sendEvent(self.viewport, event)

    def document(self, layers):
        config = copy.deepcopy(self.prototype)
        config.update(Layers=[], NextId=100000, FrozenThrough=0)
        sequence = 100
        for layer_index, operations in enumerate(layers):
            layer = {"Id": layer_index + 1, "Order": [], "Strokes": []}
            for op_index, spec in enumerate(operations):
                count, erase, mask, color, normal = spec
                stroke = copy.deepcopy(self.style)
                stroke.update(Id=20000 + layer_index * 1000 + op_index, ExplicitDabs=True,
                              Diameter=1.2, Depth=1.5, Opacity=1, NormalOpacity=normal,
                              Erase=erase, HeightMask=mask, Color=color, BlendMode=0,
                              PressureSize=False, PressureOpacity=False, Points=[])
                for _ in range(count):
                    p = copy.deepcopy(self.point)
                    p.update(Sequence=sequence, Pressure=1)
                    sequence += 1
                    stroke["Points"].append(p)
                layer["Strokes"].append(stroke)
                layer["Order"].append(stroke["Id"])
            config["Layers"].append(layer)
        return config

    def hide(self, canvas):
        if canvas is not None:
            config = data(canvas)
            for layer in config.get("Layers", []):
                layer["Visible"] = False
            put(canvas, config)

    def prepare_cases(self):
        green, red, blue = [.02, .8, .06], [.8, .03, .02], [.02, .05, .8]
        paint = lambda mask=7, color=green, normal=0: (150, False, mask, color, normal)
        erase = lambda mask=7, count=160: (count, True, mask, green, 0)
        self.cases = [("paint-all", [[paint()]])]
        for name, mask in [("low", 1), ("medium", 2), ("high", 4), ("low-high", 5), ("none", 0)]:
            self.cases.append(("paint-" + name, [[paint(mask)]]))
        for name, mask in [("low", 1), ("medium", 2), ("high", 4), ("all", 7), ("none", 0)]:
            self.cases.append(("erase-" + name, [[paint(), erase(mask)]]))
            if name == "high":self.cases.append(("erase-high-roundtrip", None))
        self.cases += [
            ("lower-only", [[paint(color=red)]]),
            ("lower-protected", [[paint(color=red)], [paint(), erase()]]),
            ("blue-only", [[paint(color=blue)]]),
            ("later-paint", [[paint(), erase(), paint(color=blue)]]),
            ("normal-paint", [[paint(normal=.8)]]),
            ("normal-erased", [[paint(normal=.8), erase()]]),
            ("mixed-erase-spatial", [[paint(normal=.3), erase(4), paint(1, blue)]]),
            ("mixed-erase-reference", [[paint(normal=.3), erase(4), paint(1, blue)]]),
            ("alpha-erase-spatial", [[paint(), erase(2), paint(1, blue)]]),
            ("alpha-erase-reference", [[paint(), erase(2), paint(1, blue)]]),
        ]

    def pixels(self, name):
        img = QtGui.QImage(str(output / (name + ".png")))
        assert not img.isNull(), name
        return [img.pixelColor(x, y) for y in range(90, 300, 2) for x in range(170, 470, 2)]

    def differences(self, a, b):
        return [max(abs(x.red()-y.red()), abs(x.green()-y.green()), abs(x.blue()-y.blue())) for x, y in zip(a, b)]

    def verify_images(self):
        baseline, all_paint = self.pixels("baseline"), self.pixels("paint-all")
        delta = self.differences(baseline, all_paint)
        painted = [i for i, d in enumerate(delta) if d > 12]
        assert len(painted) > 500, ("Paint not visible", len(painted))
        masks = []
        for band in ["low", "medium", "high"]:
            image = self.pixels("paint-" + band)
            mask = {i for i, d in enumerate(self.differences(baseline, image)) if d > 8}
            assert len(mask) > 20, ("Height band not visible", band, len(mask))
            masks.append(mask)
        union = set.union(*masks)
        coverage = sum(i in union for i in painted) / len(painted)
        overlap = len((masks[0] & masks[1]) | (masks[0] & masks[2]) | (masks[1] & masks[2])) / len(painted)
        assert coverage > .95 and overlap < .08, (coverage, overlap)
        result["height_partition"] = {"band_pixels": list(map(len, masks)), "coverage": coverage, "overlap": overlap}
        for a, b in [("paint-none", "baseline"), ("erase-none", "paint-all"),
                     ("erase-high-roundtrip", "erase-high"),
                     ("lower-protected", "lower-only"), ("later-paint", "blue-only"),
                     ("mixed-erase-spatial", "mixed-erase-reference"),
                     ("alpha-erase-spatial", "alpha-erase-reference")]:
            errors = self.differences(self.pixels(a), self.pixels(b))
            mean = sum(errors) / len(errors)
            result.setdefault("comparisons", {})[a + " vs " + b] = {"mean": mean, "max": max(errors)}
            assert mean < 2, (a, b, mean)
        # Restricted erase must retain paint outside its own selected height band.
        for index, band in enumerate(["low", "medium", "high"]):
            erased = self.pixels("erase-" + band)
            removed = {i for i, d in enumerate(self.differences(erased, all_paint)) if d > 8}
            assert len(removed & masks[index]) > 10, ("Erase did not affect its selected band", band)
            outside = [i for i in painted if i not in masks[index] and i not in removed]
            assert len(outside) > 20, ("Erase also removed unselected bands", band)
        for name in ["erase-all", "normal-erased"]:
            errors = self.differences(self.pixels(name), baseline)
            residual = sum(errors) / len(errors)
            original = sum(delta) / len(delta)
            assert residual < original * .15, ("Erase did not restore underlying surface", name, residual, original)
        result["image_checks"] = True

    def advance(self):
        try:
            self.pulses += 1
            assert time.monotonic() - self.started < 300, "Erase/height verification timed out"
            x, y = 320, 180
            if self.step == 0:
                result["stage"] = "waiting for SILPOM depth"
                if silpom.DecalPainterBus(bus.Broadcast, "PickVisiblePixel", x, y).GetLength() < 1:
                    flush();return
                self.capture("baseline");self.step = -1
            elif self.step == -1:
                if not self.captured():return
                click(self.viewport, x, y);self.step = 1
            elif self.step == 1:
                if not count(self.canvas)[0]:click(self.viewport, x, y);return
                if silpom.DecalPaintRequestBus(bus.Event, "GetRenderHandleCount", self.canvas) != 1:return
                self.prototype = data(self.canvas)
                self.paint_world = components.TransformBus(bus.Event, "GetWorldTM", self.canvas)
                self.style = copy.deepcopy(self.prototype["Layers"][0]["Strokes"][0])
                self.point = copy.deepcopy(self.style["Points"][0])
                self.bands("Paint", 1);self.bands("Erase", 4)
                self.operation.setCurrentIndex(0);click(self.viewport, x+20, y);self.step = 2
            elif self.step == 2:
                strokes = data(self.canvas)["Layers"][0]["Strokes"]
                assert strokes[-1].get("HeightMask", 7) == 1
                self.operation.setCurrentIndex(1);click(self.viewport, x+30, y);self.step = 3
            elif self.step == 3:
                stroke = data(self.canvas)["Layers"][0]["Strokes"][-1]
                assert stroke["Erase"] and stroke["HeightMask"] == 4
                assert self.panel.findChild(QtWidgets.QCheckBox, "PaintHeightLow").isChecked()
                assert not self.panel.findChild(QtWidgets.QCheckBox, "PaintHeightHigh").isChecked()
                general.undo();self.step = 4
            elif self.step == 4:
                assert not data(self.canvas)["Layers"][0]["Strokes"][-1].get("Erase", False)
                general.redo();self.step = 5
            elif self.step == 5:
                assert data(self.canvas)["Layers"][0]["Strokes"][-1]["Erase"]
                result["independent_controls_and_undo"] = True
                self.panel.findChild(QtWidgets.QCheckBox, "PressureSize").setChecked(True)
                self.panel.findChild(QtWidgets.QCheckBox, "PressureOpacity").setChecked(True)
                self.panel.findChild(QtWidgets.QDoubleSpinBox, "BrushDiameter").setValue(.15)
                self.panel.findChild(QtWidgets.QDoubleSpinBox, "BrushSpacing").setValue(.1)
                self.mode.setCurrentIndex(1)
                self.pen = QtGui.QPointingDevice("Synthetic erase pen", 9252, QtGui.QInputDevice.DeviceType.Stylus,
                                                QtGui.QPointingDevice.PointerType.Pen,
                                                QtGui.QInputDevice.Capability.Position | QtGui.QInputDevice.Capability.Pressure, 1, 2)
                self.pen_strokes_before = len(data(self.canvas)["Layers"][0]["Strokes"])
                self.tablet(QtCore.QEvent.Type.TabletPress, x-25, y-35, .25)
                self.tablet(QtCore.QEvent.Type.TabletMove, x+25, y-35, .8)
                self.tablet(QtCore.QEvent.Type.TabletRelease, x+25, y-35, 0)
                click(self.viewport, x, y) # Suppress driver-emulated mouse duplication.
                self.step = 6
            elif self.step == 6:
                strokes = data(self.canvas)["Layers"][0]["Strokes"]
                assert len(strokes) == self.pen_strokes_before + 1
                pen_stroke = strokes[-1]
                pressures = [p.get("Pressure", 1) for p in pen_stroke["Points"]]
                assert pen_stroke["Erase"] and pen_stroke["HeightMask"] == 4 and pen_stroke["PressureOpacity"]
                assert min(pressures) < .3 and max(pressures) > .6, pressures
                result["erase_pen_pressure"] = [min(pressures), max(pressures)]
                self.hide(self.canvas);self.prepare_cases();self.step = 10
            elif self.step == 10:
                self.hide(self.current_canvas)
                if self.case_index == len(self.cases):
                    self.verify_images();self.step = 20;flush();return
                name, layers = self.cases[self.case_index]
                result["stage"] = "rendering " + name
                general.run_console("r_silpomPaintCompositeReference " + ("true" if name.endswith("-reference") else "false"))
                self.current_canvas = self.new_canvas()
                put(self.current_canvas, self.saved_erase if layers is None else self.document(layers))
                self.wait_until = time.monotonic() + 1.5;self.step = 11
            elif self.step == 11:
                if time.monotonic() < self.wait_until:return
                self.capture(self.cases[self.case_index][0]);self.step = 12
            elif self.step == 12:
                if not self.captured():return
                doc = data(self.current_canvas)
                fixed = [p for l in doc.get("Layers", []) for p in l.get("Composite", [])]
                name = self.cases[self.case_index][0]
                if name == "erase-high":
                    assert any(p.get("Erase") and p.get("HeightMask") == 4 for p in fixed)
                    result["filtered_erase_in_composite"] = True
                    encoded = silpom.DecalPaintRequestBus(bus.Event, "GetDocumentJson", self.current_canvas)
                    assert '"HeightMask"' in encoded and '"Erase"' in encoded
                    (output / "filtered-erase.json").write_text(encoded, encoding="utf-8")
                    self.saved_erase = copy.deepcopy(doc)
                self.case_index += 1;self.step = 10
            elif self.step == 20:
                general.run_console("r_silpomPaintCompositeReference false")
                # Creating a prefab can replace entity IDs asynchronously. Do
                # this after comparisons, when the old canvas is no longer used.
                saved_canvas = self.new_canvas()
                put(saved_canvas, self.saved_erase)
                error = silpom.DecalPainterBus(bus.Broadcast, "SavePaintPrefab", saved_canvas, str(output / "filtered-erase.prefab"))
                assert not error, error
                prefab = (output / "filtered-erase.prefab").read_text(encoding="utf-8")
                assert '"Erase"' in prefab and '"HeightMask"' in prefab
                result["filtered_erase_prefab_saved"] = True
                assert hashlib.sha256(level.read_bytes()).hexdigest() == result["level_sha256"]
                assert "Trace::Assert" not in (project / "user/log/Editor.log").read_text(encoding="utf-8", errors="replace")
                result.update(passed=True, stage="complete", editor_assertions=0)
                self.timer.stop();flush();QtCore.QTimer.singleShot(200, finish)
            flush()
        except Exception:
            result["error"] = traceback.format_exc()
            self.timer.stop();general.run_console("r_silpomPaintCompositeReference false");flush()
            QtCore.QTimer.singleShot(100, finish)


# Run the shared startup with this test's QTimer state machine.
Check = EraseCheck
exec("try:\n    flush()" + startup, globals())

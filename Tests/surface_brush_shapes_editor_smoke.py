"""Procedural surface brushes through the native rasterizer and Editor dock.

Run in a dedicated rebuilt Editor --runpython session. It never saves the
level or enters gameplay. Authoritative source texels, rather than projected
dab history or screenshots, verify shape coverage, fresh random edges and
exact undo/redo/serialization. Existing paint documents are restored on exit.
"""
import array
import base64
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import time
import traceback

import azlmbr.bus as bus
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.math as azmath
import azlmbr.paths as paths
import azlmbr.silpom as silpom
from PySide6 import QtCore, QtGui, QtWidgets

PROJECT = Path(paths.projectroot)
LEVEL = PROJECT / 'Levels/VueLiveLink/VueLiveLink.prefab'
OUTPUT = Path(os.environ.get('TG_SILPOM_ESTIMATOR_OUTPUT', str(PROJECT / 'user/SurfaceBrushShapesSmoke')))
OUTPUT.mkdir(parents=True, exist_ok=True)
SHAPES = ('Texture stamp', 'Hard round', 'Soft round', 'Hard square', 'Soft square', 'Random edge round')
result = {'passed': False, 'stage': 'startup', 'entered_gameplay': False}
hidden_documents = []
canvas = None
empty = None


def stage(name):
    result['stage'] = name
    (OUTPUT / 'results.json').write_text(json.dumps(result, indent=2) + '\n')


def wait(predicate, seconds=120):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        general.idle_wait_frames(1)
    raise TimeoutError(result['stage'])


def document():
    raw = silpom.DecalPaintRequestBus(bus.Event, 'GetDocumentJson', canvas)
    assert raw, 'Paint document unavailable'
    return json.loads(raw)


def charts(doc):
    return doc['ClassData'].get('SurfacePaint', [])


def put(doc, target=None):
    error = silpom.DecalPainterBus(bus.Broadcast, 'SetPaintDocument', target or canvas, json.dumps(doc))
    assert not error, error
    general.idle_wait_frames(8)


def no_dabs(doc):
    assert not any(layer.get('Strokes') or layer.get('Composite') for layer in doc['ClassData'].get('Layers', []))
    assert silpom.DecalPaintRequestBus(bus.Event, 'GetDabCount', canvas) == 0
    assert silpom.DecalPaintRequestBus(bus.Event, 'GetCompositeDabCount', canvas) == 0


def source_pixels(chart):
    data = base64.b64decode(chart['Texels'], validate=True)
    magic, width, height, count, checksum = struct.unpack_from('<5I', data)
    assert magic == 0x31545053 and count == width * height * 8
    words = array.array('I')
    offset = 20
    while offset < len(data):
        token, = struct.unpack_from('<I', data, offset)
        offset += 4
        length = token & 0x7fffffff
        assert length and len(words) + length <= count
        if token & 0x80000000:
            value, = struct.unpack_from('<I', data, offset)
            offset += 4
            words.extend(array.array('I', [value]) * length)
        else:
            words.frombytes(data[offset:offset + length * 4])
            offset += length * 4
    assert offset == len(data) and len(words) == count
    values = array.array('f')
    values.frombytes(words.tobytes())
    assert all(math.isfinite(v) for v in values)
    return width, height, values


def alpha_map(doc):
    pixels = {}
    for chart in charts(doc):
        width, height, values = source_pixels(chart)
        assert width == height == 256
        assert all(v == 0 for v in values[7::8]), 'Procedural brush painted normal detail'
        # O3DE omits reflected values equal to their C++ defaults.
        key = (chart['Face'], chart.get('Layer', 0), chart.get('Front', True))
        for index, alpha in enumerate(values[3::8]):
            assert 0 <= alpha <= 1
            if alpha > 1e-7:
                pixels[key + (index,)] = alpha
    return pixels


def candidate(face):
    if face['profile'] != 1:
        return None
    corners = face['corners']
    positions = [c['position'] for c in corners]
    center = [sum(p[i] for p in positions) / 3 for i in range(3)]
    a = [positions[1][i] - positions[0][i] for i in range(3)]
    b = [positions[2][i] - positions[0][i] for i in range(3)]
    cross = (a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0])
    length = math.sqrt(sum(v*v for v in cross))
    if length <= 1e-5:
        return None
    normal = [v / length for v in cross]
    if abs(normal[2]) > .05:
        return None
    margins = []
    for k in range(3):
        edge = [positions[(k+1) % 3][i] - positions[k][i] for i in range(3)]
        delta = [center[i] - positions[k][i] for i in range(3)]
        c = (edge[1]*delta[2] - edge[2]*delta[1], edge[2]*delta[0] - edge[0]*delta[2], edge[0]*delta[1] - edge[1]*delta[0])
        edge_length = math.sqrt(sum(v*v for v in edge))
        if not edge_length:
            return None
        margins.append(math.sqrt(sum(v*v for v in c)) / edge_length)
    if min(margins) < .35:
        return None
    return {'face': face['face'], 'uv': [sum(c['uv'][i] for c in corners) / 3 for i in range(2)],
        'center': center, 'normal': normal, 'margin': min(margins)}


def stamp(shape, diameter=None, erase=False, opacity=1.):
    error = silpom.DecalPainterBus(bus.Broadcast, 'ApplySurfaceBrushStamp', canvas, tower, target['face'],
        azmath.Vector2(*target['uv']), azmath.Vector4(float(diameter or brush_diameter), float(opacity), 0., 0.),
        azmath.Color_CreateFromVector3AndFloat(azmath.Vector3(.25, .6, .85), 1.), shape, erase, 0)
    assert not error, error
    general.idle_wait_frames(8)
    doc = document()
    no_dabs(doc)
    assert charts(doc), ('No painted source texels', shape)
    return doc


def ui(cls, name):
    widget = panel.findChild(cls, name)
    assert widget is not None, name
    return widget


def click(viewport, x, y):
    local = QtCore.QPointF(x / viewport.devicePixelRatioF(), y / viewport.devicePixelRatioF())
    global_pos = viewport.mapToGlobal(local)
    for event_type, buttons in ((QtCore.QEvent.Type.MouseButtonPress, QtCore.Qt.MouseButton.LeftButton),
        (QtCore.QEvent.Type.MouseButtonRelease, QtCore.Qt.MouseButton.NoButton)):
        event = QtGui.QMouseEvent(event_type, local, global_pos, QtCore.Qt.MouseButton.LeftButton,
            buttons, QtCore.Qt.KeyboardModifier.NoModifier)
        QtWidgets.QApplication.sendEvent(viewport, event)



def run_smoke():
    global canvas, empty, tower, target, brush_diameter, panel, shape_combo
    try:
        original = LEVEL.read_bytes()
        result['level_sha256'] = hashlib.sha256(original).hexdigest()
        heights = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in (PROJECT / 'Assets/SilPOMPlanarAB').rglob('height.png')}
        assert heights
        general.idle_enable(True)
        for command in ('ed_keepEditorActive 1', 'ed_backgroundUpdatePeriod -1', 'r_silpomMsaaInspect 0'):
            general.run_console(command)
        stage('load unchanged source level')
        assert general.open_level(str(LEVEL))
        general.idle_wait_frames(120)
        query = entity.SearchFilter()
        query.names = ['Irish Fieldstone Tower (Open Shaft)']
        found = entity.SearchBus(bus.Broadcast, 'SearchEntities', query)
        assert found
        tower = found[0]
        wait(lambda: silpom.SilPomMeshRequestBus(bus.Event, 'IsReady', tower))
        faces = json.loads(silpom.SilPomMeshRequestBus(bus.Event, 'GetPaintFacesJson', tower))
        available = [c for face in faces if (c := candidate(face)) is not None]
        assert available, 'No sufficiently large supported receiving face'
        target = max(available, key=lambda c: c['margin'])
        brush_diameter = min(.8, target['margin'] * .9)
        result['receiver'] = target
        result['brush_diameter'] = brush_diameter
        stage('temporarily hide existing paint documents')
        types = editor.EditorComponentAPIBus(bus.Broadcast, 'FindComponentTypeIdsByEntityType', ['Surface Decal Paint'], entity.EntityType().Game)
        assert types
        query = entity.SearchFilter()
        query.components = {types[0]: {}}
        for other in entity.SearchBus(bus.Broadcast, 'SearchEntities', query):
            raw = silpom.DecalPaintRequestBus(bus.Event, 'GetDocumentJson', other)
            if not raw:
                continue
            saved = json.loads(raw)
            hidden_documents.append((other, saved))
            muted = copy.deepcopy(saved)
            for layer in muted['ClassData'].get('Layers', []):
                layer['Visible'] = False
            put(muted, other)
        canvas = silpom.DecalPainterBus(bus.Broadcast, 'CreatePaintEntity')
        assert canvas.IsValid()
        editor.EditorEntityAPIBus(bus.Event, 'SetName', canvas, 'Surface Brush Shapes Smoke')
        empty = document()
        assert empty['ClassData'].get('TextureAuthoritative') and not charts(empty)
        no_dabs(empty)
        panel = next(w for w in QtWidgets.QApplication.allWidgets() if w.objectName() == 'DecalPainter')
        shape_combo = ui(QtWidgets.QComboBox, 'BrushShape')
        assert shape_combo.count() == len(SHAPES)
        result['shape_choices'] = [shape_combo.itemText(i) for i in range(shape_combo.count())]
        # Canvas creation returns before the pane's tick enables controls for its
        # new authoritative document; a previously selected legacy canvas disables it.
        wait(lambda: shape_combo.isEnabled(), 10)
        for index in range(shape_combo.count()):
            shape_combo.setCurrentIndex(index)
            assert shape_combo.currentIndex() == index
        stage('hard and soft shape coverage in native source texels')
        documents = {}
        masks = {}
        for shape in range(1, 5):
            shape_combo.setCurrentIndex(shape)
            put(empty)
            documents[shape] = stamp(shape)
            masks[shape] = alpha_map(documents[shape])
            assert len(masks[shape]) > 20, (shape, len(masks[shape]))
        round_hard, round_soft, square_hard, square_soft = (masks[i] for i in range(1, 5))
        assert all(alpha <= square_hard.get(pixel, 0) + 1e-6 for pixel, alpha in round_hard.items())
        corners = {pixel: alpha for pixel, alpha in square_hard.items() if pixel not in round_hard}
        assert len(corners) > 10, ('Square failed to cover corners outside round', len(corners))
        for hard, soft in ((round_hard, round_soft), (square_hard, square_soft)):
            assert all(alpha <= hard.get(pixel, 0) + 1e-6 for pixel, alpha in soft.items())
            softened = sum(1 for pixel, alpha in soft.items() if 1e-5 < alpha < hard[pixel] - 1e-4)
            assert softened > 20, ('No soft falloff', softened)
            assert sum(soft.values()) < sum(hard.values()) * .95
        result['shape_mask_checks'] = {'painted_texels': {SHAPES[i]: len(masks[i]) for i in range(1, 5)},
            'square_corner_texels': len(corners), 'soft_falloff': True}
        stage('random edge refresh and solid center')
        put(empty)
        center_mask = alpha_map(stamp(1, diameter=brush_diameter * .35))
        assert len(center_mask) > 10
        put(empty)
        random_a = stamp(5)
        mask_a = alpha_map(random_a)
        assert all(abs(mask_a.get(pixel, 0) - alpha) < 1e-6 for pixel, alpha in center_mask.items()), 'Random brush center is not solid'
        general.undo()
        general.idle_wait_frames(8)
        assert charts(document()) == charts(empty), 'Undo failed to restore pre-random source'
        general.redo()
        general.idle_wait_frames(8)
        assert charts(document()) == charts(random_a), 'Redo recalculated random paint'
        put(empty)
        random_b = stamp(5)
        mask_b = alpha_map(random_b)
        assert all(abs(mask_b.get(pixel, 0) - alpha) < 1e-6 for pixel, alpha in center_mask.items())
        changed_edges = sum(abs(mask_a.get(pixel, 0) - mask_b.get(pixel, 0)) > 1e-6
            for pixel in set(mask_a) | set(mask_b) if pixel not in center_mask)
        assert changed_edges > 10, ('Random pattern repeated', changed_edges)
        assert all(alpha <= round_hard.get(pixel, 0) + 1e-6 for pixel, alpha in mask_a.items())
        saved_path = OUTPUT / 'random-paint-document.json'
        saved_path.write_text(json.dumps(random_a, indent=2) + '\n')
        put(empty)
        put(json.loads(saved_path.read_text()))
        assert charts(document()) == charts(random_a), 'JSON restore recalculated random paint'
        result['random_edges'] = {'changed_edge_texels': changed_edges, 'solid_center_texels': len(center_mask),
            'undo_redo_exact': True, 'json_restore_exact': True}
        stage('erase follows the selected procedural shape')
        put(documents[3])
        after_erase = alpha_map(stamp(1, erase=True))
        assert all(abs(after_erase.get(pixel, 0) - alpha) < 1e-6 for pixel, alpha in corners.items()), 'Round erase removed square corners'
        assert sum(after_erase.values()) < sum(square_hard.values()) * .9
        assert all(abs(after_erase.get(pixel, 0) - alpha * (1 - round_hard.get(pixel, 0))) < 2e-6
            for pixel, alpha in square_hard.items()), 'Erase did not use round coverage'
        result['selected_shape_erase'] = True
        stage('dock shape selection, material independence and tint preservation')
        ui(QtWidgets.QComboBox, 'PaintMode').setCurrentIndex(0)
        ui(QtWidgets.QComboBox, 'BrushOperation').setCurrentIndex(0)
        color_button = ui(QtWidgets.QPushButton, 'PaintColor')
        wait(lambda: color_button.isEnabled(), 10)
        # QColorDialog is driven in its modal event loop; the dedicated session uses
        # a Qt dialog so the test can choose a nonwhite color without user input.
        QtWidgets.QApplication.setAttribute(QtCore.Qt.ApplicationAttribute.AA_DontUseNativeDialogs, True)
        chosen_color = QtGui.QColor(61, 127, 211)
        color_set = []
        dialog_probe = {'button_enabled': color_button.isEnabled(),
            'operation_index': ui(QtWidgets.QComboBox, 'BrushOperation').currentIndex(),
            'modal_types': []}
        result['color_dialog_probe'] = dialog_probe
        color_timer = QtCore.QTimer(panel)
        color_timer.setInterval(50)
        color_deadline = time.monotonic() + 5
        def choose_color():
            modal = QtWidgets.QApplication.activeModalWidget()
            modal_type = type(modal).__name__
            if modal_type not in dialog_probe['modal_types']:
                dialog_probe['modal_types'].append(modal_type)
            # AzQtComponents reparents modal dialogs into a QFrame window
            # decoration. The QColorDialog is a child, not a top-level widget.
            dialog = modal if isinstance(modal, QtWidgets.QColorDialog) else next(
                (w for w in QtWidgets.QApplication.allWidgets() if isinstance(w, QtWidgets.QColorDialog) and w.isVisible()), None)
            if isinstance(dialog, QtWidgets.QColorDialog):
                color_timer.stop()
                dialog_probe['dialog_type'] = type(dialog).__name__
                dialog.setCurrentColor(chosen_color)
                color_set.append(True)
                dialog.accept()
            elif time.monotonic() >= color_deadline:
                color_timer.stop()
                color_set.append(False)
                if modal is not None:
                    modal.close()
        color_timer.timeout.connect(choose_color)
        color_timer.start()
        color_button.click()
        color_timer.stop()
        dialog_probe['selection_results'] = color_set
        assert color_set == [True], ('Paint color dialog unavailable', dialog_probe)
        expected_color = [chosen_color.redF(), chosen_color.greenF(), chosen_color.blueF()]
        ui(QtWidgets.QDoubleSpinBox, 'BrushDiameter').setValue(brush_diameter)
        ui(QtWidgets.QDoubleSpinBox, 'BrushOpacity').setValue(1.)
        ui(QtWidgets.QDoubleSpinBox, 'ProjectionDepth').setValue(.25)
        ui(QtWidgets.QComboBox, 'DecalBlend').setCurrentIndex(0)
        ui(QtWidgets.QLineEdit, 'DecalMaterial').setText('materials/does_not_exist_for_procedural_test.azmaterial')
        eye = [target['center'][i] + target['normal'][i] * .6 for i in range(3)]
        direction = [-v for v in target['normal']]
        general.set_current_view_position(*eye)
        general.set_current_view_rotation(math.degrees(math.atan2(direction[2], math.hypot(*direction[:2]))), 0.,
            (math.degrees(math.atan2(direction[1], direction[0])) - 90 + 180) % 360 - 180)
        viewport = QtWidgets.QWidget.find(silpom.DecalPainterBus(bus.Broadcast, 'GetViewportWindowId'))
        assert viewport is not None
        viewport.window().activateWindow()
        viewport.setFocus(QtCore.Qt.FocusReason.MouseFocusReason)
        ui(QtWidgets.QCheckBox, 'EnablePainting').setChecked(True)
        general.idle_wait_frames(60)
        size = general.get_viewport_size()
        x, y = int(size.x) // 2, int(size.y) // 2
        wait(lambda: silpom.DecalPainterBus(bus.Broadcast, 'PickVisiblePixel', x, y).GetLength() >= 1)
        for shape in range(1, 6):
            put(empty)
            shape_combo.setCurrentIndex(shape)
            assert not ui(QtWidgets.QDoubleSpinBox, 'BrushNormalOpacity').isEnabled()
            ui(QtWidgets.QComboBox, 'BrushOperation').setCurrentIndex(1)
            ui(QtWidgets.QComboBox, 'BrushOperation').setCurrentIndex(0)
            assert not ui(QtWidgets.QDoubleSpinBox, 'BrushNormalOpacity').isEnabled()
            click(viewport, x, y)
            general.idle_wait_frames(8)
            painted = document()
            no_dabs(painted)
            assert alpha_map(painted), ('Dock brush failed with missing material', shape)
            for chart in charts(painted):
                _, _, values = source_pixels(chart)
                for i in range(0, len(values), 8):
                    if values[i+3] > 1e-4:
                        assert all(abs(values[i+c] / values[i+3] - expected_color[c]) < 1e-4 for c in range(3)), ('Shape selection changed tint', shape)
        ui(QtWidgets.QCheckBox, 'EnablePainting').setChecked(False)
        library = ui(QtWidgets.QListWidget, 'DecalLibrary')
        assert library.count()
        library.itemClicked.emit(library.item(0))
        assert shape_combo.currentIndex() == 0, 'Choosing a library image did not select Texture stamp'
        result['dock_procedural_clicks'] = 5
        result['dock_preserves_selected_color'] = True
        result['procedural_no_material_dependency'] = True
        stage('legacy documents keep texture stamp behavior')
        put(empty)
        legacy = copy.deepcopy(empty)
        legacy['ClassData']['TextureAuthoritative'] = False
        put(legacy)
        general.idle_wait_frames(12)
        assert shape_combo.currentIndex() == 0 and not shape_combo.isEnabled(), 'Legacy documents exposed unsupported procedural shapes'
        put(empty)
        result['legacy_shapes_disabled'] = True
        result['retained_dabs'] = 0
        result['saved_level_unchanged'] = LEVEL.read_bytes() == original
        result['heightmaps_unchanged'] = all(hashlib.sha256(p.read_bytes()).hexdigest() == value for p, value in heights.items())
        assert result['saved_level_unchanged'] and result['heightmaps_unchanged']
        result['passed'] = True
        stage('complete')
    except Exception:
        result['error'] = traceback.format_exc()
        stage('failed: ' + result['stage'])
    finally:
        restore_errors = []
        try:
            if empty is not None:
                put(empty)
        except Exception:
            restore_errors.append(traceback.format_exc())
        for other, saved in hidden_documents:
            try:
                put(saved, other)
            except Exception:
                restore_errors.append(traceback.format_exc())
        result['other_paint_documents_restored'] = not restore_errors
        if restore_errors:
            result['passed'] = False
            result['restore_errors'] = restore_errors
        (OUTPUT / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
        QtCore.QTimer.singleShot(1000, general.exit_no_prompt)


# Leave startup Python before exercising the dock. Editor disables its parent
# window while --runpython is active; normal Qt dispatch restores it first.
QtCore.QTimer.singleShot(0, run_smoke)

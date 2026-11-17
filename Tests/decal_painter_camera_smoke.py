"""Verify independent transient painter camera light and camera collision.

Run in a dedicated rebuilt Editor --runpython session. The fixture returns
from startup before running in the Qt event loop, never enters gameplay and
never saves the level. It creates no paint or light entity.
"""
import hashlib
import json
import math
import os
from pathlib import Path
import time
import traceback

import azlmbr.atom as atom
import azlmbr.bus as bus
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.paths as paths
import azlmbr.render as render
import azlmbr.silpom as silpom
from PySide6 import QtCore, QtGui, QtWidgets

PROJECT = Path(paths.projectroot)
LEVEL = PROJECT / 'Levels/VueLiveLink/VueLiveLink.prefab'
OUTPUT = Path(os.environ.get('TG_SILPOM_ESTIMATOR_OUTPUT', str(PROJECT / 'user/DecalPainterCameraSmoke')))
OUTPUT.mkdir(parents=True, exist_ok=True)
result = {'passed': False, 'stage': 'startup', 'entered_gameplay': False}
panel = light_box = collision_box = light_lux = None
original_position = original_rotation = None


def stage(name):
    result['stage'] = name
    (OUTPUT / 'results.json').write_text(json.dumps(result, indent=2) + '\n')


def wait(predicate, seconds=30):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        general.idle_wait_frames(1)
    raise TimeoutError(result['stage'])


def vector(value):
    return [value.x, value.y, value.z]


def distance(a, b):
    return math.sqrt(sum((x-y)**2 for x, y in zip(a, b)))


def dot(a, b):
    return sum(x*y for x, y in zip(a, b))


def offset(point, direction, amount):
    return [point[i] + direction[i] * amount for i in range(3)]


def status():
    raw = silpom.DecalPainterBus(bus.Broadcast, 'GetCameraStatus')
    assert raw, 'Camera diagnostics unavailable'
    return json.loads(raw)


def toggle(light, collision):
    light_box.setChecked(light)
    collision_box.setChecked(collision)
    wait(lambda: status()['lightEnabled'] == light and status()['collisionEnabled'] == collision)
    general.idle_wait_frames(4)


def move(position, rotation=None):
    general.set_current_view_position(*position)
    if rotation is not None:
        general.set_current_view_rotation(*rotation)
    general.idle_wait_frames(4)
    return vector(general.get_current_view_position())


def candidate(face):
    positions = [corner['position'] for corner in face['corners']]
    if len(positions) != 3:
        return None
    a = [positions[1][i] - positions[0][i] for i in range(3)]
    b = [positions[2][i] - positions[0][i] for i in range(3)]
    cross = [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]
    length = math.sqrt(dot(cross, cross))
    if length < 1e-5:
        return None
    normal = [v / length for v in cross]
    if abs(normal[2]) > .05:
        return None
    center = [sum(p[i] for p in positions) / 3 for i in range(3)]
    margin = min(length / (3 * distance(positions[k], positions[(k+1) % 3])) for k in range(3))
    if margin < .35:
        return None
    return {'face': face['face'], 'center': center, 'normal': normal, 'margin': margin}


def paint_documents():
    types = editor.EditorComponentAPIBus(bus.Broadcast, 'FindComponentTypeIdsByEntityType',
        ['Surface Decal Paint'], entity.EntityType().Game)
    assert types
    query = entity.SearchFilter()
    query.components = {types[0]: {}}
    return {e.ToString(): silpom.DecalPaintRequestBus(bus.Event, 'GetDocumentJson', e)
        for e in entity.SearchBus(bus.Broadcast, 'SearchEntities', query)}


def entity_metadata(identity):
    """Resolve transient scene IDs before this dedicated Editor is closed."""
    details = {'requested_id': identity, 'ancestry': []}
    try:
        entities = entity.SearchBus(bus.Broadcast, 'SearchEntities', entity.SearchFilter())
        current = next((item for item in entities if item.ToString() == identity), None)
        mesh_bus = getattr(render, 'RenderMeshComponentRequestBus', None) or getattr(render, 'MeshComponentRequestBus', None)
        details['mesh_bus_available'] = callable(mesh_bus)
        seen = set()
        while current is not None and current.IsValid() and current.ToString() not in seen:
            seen.add(current.ToString())
            item = {'id': current.ToString(),
                'name': editor.EditorEntityInfoRequestBus(bus.Event, 'GetName', current),
                'editor_visible': editor.EditorEntityInfoRequestBus(bus.Event, 'IsVisible', current),
                'editor_hidden': editor.EditorEntityInfoRequestBus(bus.Event, 'IsHidden', current)}
            details['ancestry'].append(item)
            if callable(mesh_bus):
                model = mesh_bus(bus.Event, 'GetModelAssetPath', current)
                if model:
                    item['model_asset'] = model
                    item['mesh_visible'] = mesh_bus(bus.Event, 'GetVisibility', current)
            current = editor.EditorEntityInfoRequestBus(bus.Event, 'GetParent', current)
    except Exception:
        # Optional diagnostics must not prevent the independent camera checks.
        details['lookup_error'] = traceback.format_exc()
    return details


def capture(name):
    path = OUTPUT / name
    request = atom.FrameCaptureRequestBus(bus.Broadcast, 'CaptureScreenshot', str(path))
    assert request.IsSuccess(), ('Frame capture unavailable', name)
    done = {}
    handler = atom.FrameCaptureNotificationBusHandler()
    handler.connect(request.GetValue())
    handler.add_callback('OnFrameCaptureFinished', lambda args: done.update(success=args[0] == atom.FrameCaptureResult_Success))
    try:
        wait(lambda: bool(done))
        assert done['success'], ('Frame capture failed', name)
    finally:
        handler.disconnect()
    image = QtGui.QImage(str(path))
    assert not image.isNull()
    samples = [image.pixelColor(x, y) for y in range(image.height()*4//10, image.height()*6//10, 4)
        for x in range(image.width()*4//10, image.width()*6//10, 4)]
    assert samples
    return {'mean_rgb_sum': sum(c.red()+c.green()+c.blue() for c in samples) / len(samples),
        'clipped_fraction': sum(max(c.red(), c.green(), c.blue()) >= 254 for c in samples) / len(samples)}


def run_smoke():
    global panel, light_box, collision_box, light_lux, original_position, original_rotation
    try:
        original_level = LEVEL.read_bytes()
        result['level_sha256'] = hashlib.sha256(original_level).hexdigest()
        general.idle_enable(True)
        for command in ('ed_keepEditorActive 1', 'ed_backgroundUpdatePeriod -1'):
            general.run_console(command)
        stage('load unchanged source level')
        assert general.open_level(str(LEVEL))
        general.idle_wait_frames(120)
        original_position = vector(general.get_current_view_position())
        original_rotation = vector(general.get_current_view_rotation())
        original_documents = paint_documents()
        entity_count = len(entity.SearchBus(bus.Broadcast, 'SearchEntities', entity.SearchFilter()))
        query = entity.SearchFilter()
        query.names = ['Irish Fieldstone Tower (Open Shaft)']
        found = entity.SearchBus(bus.Broadcast, 'SearchEntities', query)
        assert found, 'Tower fixture unavailable'
        receiver = found[0]
        wait(lambda: silpom.SilPomMeshRequestBus(bus.Event, 'IsReady', receiver), 120)
        faces = json.loads(silpom.SilPomMeshRequestBus(bus.Event, 'GetPaintFacesJson', receiver))
        candidates = [item for face in faces if (item := candidate(face)) is not None]
        assert candidates, 'No broad vertical wall for collision/light verification'
        wall = max(candidates, key=lambda item: item['margin'])
        result['wall'] = wall
        # Keep the full camera sphere clear in the narrow corridor between the
        # wall and the central core.
        outside = offset(wall['center'], wall['normal'], .30)
        inside = offset(wall['center'], wall['normal'], -.8)
        stage('independent camera toggles without painting')
        silpom.DecalPainterBus(bus.Broadcast, 'OpenPainter')
        wait(lambda: any(w.objectName() == 'DecalPainter' and w.isVisible() for w in QtWidgets.QApplication.allWidgets()))
        panel = next(w for w in QtWidgets.QApplication.allWidgets() if w.objectName() == 'DecalPainter')
        light_box = panel.findChild(QtWidgets.QCheckBox, 'CameraLight')
        collision_box = panel.findChild(QtWidgets.QCheckBox, 'CameraCollision')
        light_lux = panel.findChild(QtWidgets.QDoubleSpinBox, 'CameraLightLux')
        arm = panel.findChild(QtWidgets.QCheckBox, 'EnablePainting')
        assert light_box is not None and collision_box is not None and light_lux is not None and arm is not None
        assert light_lux.minimum() == 0 and light_lux.maximum() == 1e6
        assert light_lux.value() == 10 and status()['lightLux'] == 10
        arm.setChecked(False)
        toggle(False, False)
        assert light_lux.isEnabled(), 'Light brightness must remain editable while off'
        light_lux.setValue(250)
        general.idle_wait_frames(4)
        state = status()
        assert state['lightLux'] == 250 and not state['lightEnabled'] and not state['collisionEnabled'], state
        assert not state['lightActive'] and not state['collisionActive'], state
        light_lux.setValue(10)
        move(outside)
        for light, collision in ((False, False), (True, False), (False, True), (True, True)):
            toggle(light, collision)
            state = status()
            assert state['lightActive'] == light, state
            assert state['collisionActive'] == collision, state
            assert not arm.isChecked()
        result['independent_toggle_combinations'] = 4
        result['light_brightness_adjustment_while_off_keeps_toggles_off'] = True
        stage('camera light follows position and orientation')
        toggle(True, False)
        position = move(outside, [0., 0., 0.])
        state = status()
        assert distance(state['lightPosition'], position) < .005, state
        assert distance(state['lightDirection'], [0., 1., 0.]) < .02, state
        moved = offset(outside, [0., 0., 1.], .2)
        position = move(moved, [0., 0., 90.])
        state = status()
        assert distance(state['lightPosition'], position) < .005, state
        assert distance(state['lightDirection'], [-1., 0., 0.]) < .02, state
        light_lux.setValue(2000)
        general.idle_wait_frames(4)
        state = status()
        assert state['lightLux'] == 2000 and state['lightEnabled'] and state['lightActive'] and not state['collisionEnabled'], state
        light_lux.setValue(10)
        result['active_light_brightness_adjustable'] = True
        result['light_follows_camera_position_and_direction'] = True
        stage('camera collision blocks crossing and disabled mode flies freely')
        toggle(False, False)
        assert distance(move(outside), outside) < .005
        toggle(False, True)
        result['collision_free_motion'] = []
        tangent = [-wall['normal'][1], wall['normal'][0], 0.]
        free_moves = [('approach', offset(wall['center'], wall['normal'], .25)),
            ('back_away', outside), ('along_wall', offset(outside, tangent, .025)), ('return', outside)]
        for name, target in free_moves:
            actual = move(target)
            state = status()
            details = {'motion': name, 'requested': target, 'actual': actual, 'state': state,
                'hit_entity': entity_metadata(state['hitEntity'])}
            result['collision_free_motion'].append(details)
            if distance(actual, target) >= .005:
                result.setdefault('check_failures', []).append({'check': 'Collision constrained clear movement', 'details': details})
                break
        # Run the crossing and the independent light/lifetime checks even after
        # a clear-motion failure, retaining a failed final result for that run.
        toggle(False, False)
        move(outside)
        toggle(False, True)
        before = status()
        blocked_position = move(inside)
        after = status()
        signed_distance = dot([blocked_position[i]-wall['center'][i] for i in range(3)], wall['normal'])
        assert signed_distance > .02, ('Camera crossed the wall', blocked_position, signed_distance, after)
        assert distance(blocked_position, inside) > .3
        assert after['blockedCount'] > before['blockedCount'], after
        toggle(False, False)
        free_position = move(inside)
        assert distance(free_position, inside) < .005, ('Disabled collision still constrained camera', free_position)
        result['collision'] = {'blocked_position': blocked_position, 'signed_wall_distance': signed_distance,
            'free_position': free_position, 'enabled_blocks_disabled_crosses': True,
            'before_status': before, 'after_status': after,
            'hit_entity': entity_metadata(after['hitEntity'])}
        stage('headlight visibly illuminates the receiving wall')
        direction = [-value for value in wall['normal']]
        look_rotation = [math.degrees(math.atan2(direction[2], math.hypot(*direction[:2]))), 0.,
            (math.degrees(math.atan2(direction[1], direction[0])) - 90 + 180) % 360 - 180]
        # Leave space for the finite-radius light and avoid a clipped close-up
        # when comparing two brightness values at an otherwise identical pose.
        light_camera = offset(wall['center'], wall['normal'], 2.)
        move(light_camera, look_rotation)
        general.idle_wait_frames(30)
        unlit = capture('camera-light-off.png')
        light_lux.setValue(1)
        toggle(True, False)
        general.idle_wait_frames(30)
        low = capture('camera-light-1-lux.png')
        assert status()['lightLux'] == 1
        light_lux.setValue(10)
        general.idle_wait_frames(30)
        high = capture('camera-light-10-lux.png')
        assert status()['lightLux'] == 10
        assert low['mean_rgb_sum'] > unlit['mean_rgb_sum'] + 2, ('Camera light did not brighten the wall', unlit, low)
        assert high['mean_rgb_sum'] > low['mean_rgb_sum'] + 2, ('Higher lux did not brighten the wall', low, high)
        assert high['clipped_fraction'] < .1, ('Brightness comparison clipped the receiving wall', high)
        result['light_render'] = {'camera_position': light_camera, 'off': unlit, '1_lux': low, '10_lux': high}
        light_lux.setValue(10)
        stage('hiding the painter releases transient camera features')
        toggle(True, True)
        dock = panel.parentWidget()
        ancestors = []
        ancestor = panel
        while ancestor is not None:
            ancestors.append({'type': ancestor.metaObject().className(), 'name': ancestor.objectName(), 'visible': ancestor.isVisible()})
            ancestor = ancestor.parentWidget()
        result['pane_ancestors_before_hide'] = ancestors
        while dock is not None and not isinstance(dock, QtWidgets.QDockWidget):
            dock = dock.parentWidget()
        hidden = dock or panel
        hidden.hide()
        wait(lambda: not status()['lightActive'] and not status()['collisionActive'])
        assert status()['lightEnabled'] and status()['collisionEnabled'], 'Hiding erased independent selections'
        # OpenPane may show an outer fancy-docking wrapper while leaving an
        # explicitly hidden inner QDockWidget unchanged; restore the same widget.
        hidden.show()
        silpom.DecalPainterBus(bus.Broadcast, 'OpenPainter')
        try:
            wait(lambda: panel.isVisible() and status()['lightActive'] and status()['collisionActive'])
        except Exception:
            result['reopen_failure'] = {'panel_visible': panel.isVisible(), 'hidden_widget_visible': hidden.isVisible(), 'state': status()}
            raise
        toggle(False, False)
        result['hidden_features_released_and_visible_features_restored'] = True
        result['no_scene_entities_created'] = entity_count == len(entity.SearchBus(bus.Broadcast, 'SearchEntities', entity.SearchFilter()))
        result['paint_documents_unchanged'] = paint_documents() == original_documents
        result['saved_level_unchanged'] = LEVEL.read_bytes() == original_level
        assert result['no_scene_entities_created'] and result['paint_documents_unchanged'] and result['saved_level_unchanged']
        assert not result.get('check_failures'), result.get('check_failures')
        result['passed'] = True
        stage('complete')
    except Exception:
        result['error'] = traceback.format_exc()
        stage('failed: ' + result['stage'])
    finally:
        try:
            if light_box is not None and collision_box is not None:
                toggle(False, False)
            if light_lux is not None:
                light_lux.setValue(10)
            if original_position is not None:
                move(original_position, original_rotation)
            state = status() if panel is not None else {}
            result['camera_features_released'] = not state.get('lightActive', False) and not state.get('collisionActive', False)
        except Exception:
            result['cleanup_error'] = traceback.format_exc()
            result['passed'] = False
        (OUTPUT / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
        QtCore.QTimer.singleShot(500, general.exit_no_prompt)


# Editor disables its main window during startup Python; exercise controls only
# after that call returns and normal Qt event dispatch resumes.
QtCore.QTimer.singleShot(0, run_smoke)

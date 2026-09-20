"""Recreate issue #6, then isolate actual components in an UNSAVED DefaultLevel.

Launch with the same Editor arguments as planar_mesh_editor_smoke.py.
SILPOM_FACE_OUTPUT selects a fresh capture directory; SILPOM_FACE_EXIT=1 exits.
Capture success is not visual acceptance: inspect baseline before comparisons.
"""
from pathlib import Path
import os
import runpy

# Run the normal fixture/lifecycle checks first, retaining its unsaved scene and
# capture helpers. Temporarily suppress its exit so isolation can follow.
exit_requested = os.environ.get('SILPOM_FACE_EXIT')
os.environ['SILPOM_FACE_EXIT'] = '0'
try:
    fixture = runpy.run_path(str(Path(__file__).with_name('planar_mesh_editor_smoke.py')))
finally:
    if exit_requested is None:
        os.environ.pop('SILPOM_FACE_EXIT', None)
    else:
        os.environ['SILPOM_FACE_EXIT'] = exit_requested
assert fixture['result']['passed'], fixture['result'].get('error')
globals().update({k: v for k, v in fixture.items() if not k.startswith('__')})


def find_components(name, type_name):
    search = entity.SearchFilter()
    if name:
        search.names = [name]
    types = editor.EditorComponentAPIBus(bus.Broadcast, 'FindComponentTypeIdsByEntityType',
                                       [type_name], entity.EntityType().Game)
    found = []
    for target in entity.SearchBus(bus.Broadcast, 'SearchEntities', search):
        for type_id in types:
            outcome = editor.EditorComponentAPIBus(bus.Broadcast, 'GetComponentsOfType', target, type_id)
            if outcome.IsSuccess():
                found.extend(outcome.GetValue())
    return found


def enabled(comp):
    return editor.EditorComponentAPIBus(bus.Broadcast, 'IsComponentEnabled', comp)


def set_enabled(comps, value):
    assert editor.EditorComponentAPIBus(bus.Broadcast, 'EnableComponents' if value else 'DisableComponents', comps)
    general.idle_wait_frames(60)
    assert all(enabled(comp) == value for comp in comps)


def capture_attachment(name, hierarchy, slot):
    filename = str(output / (name + '.dds'))
    request = atom.FrameCaptureRequestBus(bus.Broadcast, 'CapturePassAttachment', filename, hierarchy, slot, 0)
    assert request.IsSuccess(), name
    done = {}
    handler = atom.FrameCaptureNotificationBusHandler()
    handler.connect(request.GetValue())
    handler.add_callback('OnFrameCaptureFinished', lambda args: done.update(success=args[0] == atom.FrameCaptureResult_Success))
    for _ in range(300):
        if done:
            break
        general.idle_wait_frames(1)
    handler.disconnect()
    assert done.get('success'), filename
    result['captures'].append(filename)
    flush()


def run():
    result['passed'] = False
    result['scope'] = 'Component/shadow isolation; baseline and comparisons require visual review'
    general.set_current_view_position(18.5, 13.5, 40.2)
    general.set_current_view_rotation(0.0, 0.0, 45.0)
    general.idle_wait_frames(90)
    capture('baseline')
    if os.environ.get('SILPOM_TERRAIN_STUDY') == '1':
        capture_attachment('shadowmap', ['MainPipeline_0', 'Shadows', 'Cascades'], 'Shadowmap')
        capture_attachment('fullscreen_shadow', ['MainPipeline_0', 'Shadows', 'FullscreenShadowPass'], 'Output')
        all_meshes = []
        for kind in ['Mesh', 'SilPOM Mesh', 'SilPOM Patch']:
            all_meshes.extend(comp for comp in find_components(None, kind) if enabled(comp))
        result['disabled_mesh_count'] = len(all_meshes)
        set_enabled(all_meshes, False)
        capture('all_meshes_disabled')
        capture_attachment('shadowmap_no_meshes', ['MainPipeline_0', 'Shadows', 'Cascades'], 'Shadowmap')
        set_enabled(all_meshes, True)
        for setting, values in [('ShadowBias', [.05, .2, 1.0]), ('NormalShadowBias', [0.0, 2.5, 10.0]),
                                ('ShadowmapSize', [2048])]:
            old = render.DirectionalLightRequestBus(bus.Broadcast, 'Get' + setting)
            result[setting] = old
            for value in values:
                render.DirectionalLightRequestBus(bus.Broadcast, 'Set' + setting, value)
                capture(setting + '_' + str(value))
            render.DirectionalLightRequestBus(bus.Broadcast, 'Set' + setting, old)
        result['passed'] = True
        return
    result['components'] = {}
    for name, kind in [('SilPOM_Planar_FBX_Edge_Study', 'SilPOM Mesh'),
                       ('Grid_001', 'Mesh'), ('silpom test', 'SilPOM Patch'),
                       ('SilPOM_Planar_AB_UNSAVED', 'SilPOM Patch')]:
        comps = [comp for comp in find_components(name, kind) if enabled(comp)]
        result['components'][name] = len(comps)
        assert comps, name
        set_enabled(comps, False)
        capture('disabled_' + name.replace(' ', '_'))
        set_enabled(comps, True)
        capture('restored_' + name.replace(' ', '_'))
    lights = find_components(None, 'Directional Light')
    assert len(lights) == 1, len(lights)
    light = lights[0]
    properties = editor.EditorComponentAPIBus(bus.Broadcast, 'BuildComponentPropertyList', light)
    result['light_properties'] = list(properties)
    shadow_path = next(p for p in properties if p.endswith('|Enable Shadow'))
    old_shadow = editor.EditorComponentAPIBus(bus.Broadcast, 'GetComponentProperty', light, shadow_path).GetValue()
    for value, name in [(False, 'directional_shadows_off'), (old_shadow, 'directional_shadows_restored')]:
        outcome = editor.EditorComponentAPIBus(bus.Broadcast, 'SetComponentProperty', light, shadow_path, value)
        assert outcome.IsSuccess()
        capture(name)
    for setting, value, name in [('ShadowFilterMethod', 0, 'unfiltered'),
                                 ('ShadowReceiverPlaneBiasEnabled', False, 'receiver_bias_off'),
                                 ('ShadowBias', .0015, 'shadow_bias_0015')]:
        old = render.DirectionalLightRequestBus(bus.Broadcast, 'Get' + setting)
        result[setting] = old
        render.DirectionalLightRequestBus(bus.Broadcast, 'Set' + setting, value)
        capture(name)
        render.DirectionalLightRequestBus(bus.Broadcast, 'Set' + setting, old)
    capture('restored')
    result['passed'] = True


try:
    run()
except Exception:
    result['error'] = traceback.format_exc()
finally:
    flush()
    print(json.dumps(result, indent=2))
    if os.environ.get('SILPOM_FACE_EXIT') == '1':
        general.exit_no_prompt()

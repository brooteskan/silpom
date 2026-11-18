"""Capture gameplay wall coverage and depth equality without saving the source level."""
from pathlib import Path
helper = Path(__file__).with_name('LightingEstimatorEditorSmoke.py')
exec(compile(helper.read_text().split('def main():')[0], str(helper), 'exec'))
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import silpom_editor_paths as silpom_paths
import azlmbr.components as components
import azlmbr.math as azmath
from PySide6 import QtCore
SOURCE_LEVEL = LEVEL
LEVEL = PROJECT / 'Levels/DecalCoverageDiagnostic/DecalCoverageDiagnostic.prefab'
if not LEVEL.exists():
    LEVEL.parent.mkdir(parents=True, exist_ok=True)
    LEVEL.write_bytes(SOURCE_LEVEL.read_bytes())
source = LEVEL.read_bytes()


def capture_attachment(path, hierarchy, slot):
    done = []
    handler = atom.FrameCaptureNotificationBusHandler()
    request = atom.FrameCaptureRequestBus(bus.Broadcast, 'CapturePassAttachment', str(path), hierarchy, slot, 1)
    assert request.IsSuccess(), str(request.GetError())
    handler.connect(request.GetValue())
    handler.add_callback('OnFrameCaptureFinished', lambda args: done.append(args[0]))
    deadline = time.monotonic() + 45
    while not done and time.monotonic() < deadline:
        general.idle_wait_frames(1)
    handler.disconnect()
    assert done == [atom.FrameCaptureResult_Success], done


def game_view(pose):
    active = general.find_game_entity('Camera')
    rotation = azmath.Quaternion_CreateFromEulerAnglesDegrees(azmath.Vector3(*map(float, pose['rotation'])))
    transform = azmath.Transform_CreateFromQuaternionAndTranslation(rotation, azmath.Vector3(*map(float, pose['position'])))
    components.TransformBus(bus.Event, 'SetWorldTM', active, transform)
    general.set_current_view_position(*pose['position'])
    general.set_current_view_rotation(*pose['rotation'])
    general.idle_wait_frames(30)
    actual = general.get_current_view_position()
    result.setdefault('cameras', []).append({'requested': pose, 'actual': [actual.x, actual.y, actual.z]})
    assert max(abs(a-b) for a, b in zip(pose['position'], [actual.x, actual.y, actual.z])) < .01, result['cameras'][-1]


def main():
    global tower
    source = LEVEL.read_bytes()
    result['level_sha256'] = hashlib.sha256(source).hexdigest()
    general.idle_enable(True)
    for command in ('ed_keepEditorActive 1', 'ed_backgroundUpdatePeriod -1', 'r_displayInfo 0'):
        general.run_console(command)
    shader_source = silpom_paths.GEM/'Assets/Shaders/SilPOM/HitReuse/SilPOMClassifyWallHoles.azsl'
    shader_product = PROJECT/'Cache/pc/shaders/silpom/hitreuse/silpomclassifywallholes_dx12_0.azshadervariant'
    result['stage'] = 'waiting for wall-recovery shader'; flush()
    deadline = time.monotonic()+180
    while not shader_product.exists() or shader_product.stat().st_mtime < shader_source.stat().st_mtime:
        assert time.monotonic() < deadline, 'Wall-recovery shader did not rebuild'
        general.idle_wait_frames(1)
    backup_source = silpom_paths.GEM/'Assets/Materials/Pipelines/MainPipeline/SilPOMHitDepth.azsli'
    backup_products = [PROJECT/('Cache/pc/materials/types/silpommesh/silpommesh_mainpipeline_silpomhitdepthwallbackup'+suffix+'_dx12_0.azshadervariant')
        for suffix in ('', '-nomsaa')]
    result['stage'] = 'waiting for exact-depth backup shader'; flush()
    deadline = time.monotonic()+300
    while any(not p.exists() or p.stat().st_mtime < backup_source.stat().st_mtime for p in backup_products):
        assert time.monotonic() < deadline, 'Backup depth shader did not rebuild'
        general.idle_wait_frames(1)
    assert general.open_level(str(LEVEL))
    general.idle_wait_frames(90)
    query = entity.SearchFilter(); query.names = ['Irish Fieldstone Tower (Open Shaft)']
    tower = entity.SearchBus(bus.Broadcast, 'SearchEntities', query)[0]
    ready()
    # The saved source need not contain the user's unsaved paint. Explicitly
    # disable any saved paint too, so this is a no-decal coverage control.
    types = editor.EditorComponentAPIBus(bus.Broadcast, 'FindComponentTypeIdsByEntityType', ['Surface Decal Paint'], entity.EntityType().Game)
    query = entity.SearchFilter(); query.components = {types[0]: {}}
    paints = entity.SearchBus(bus.Broadcast, 'SearchEntities', query)
    for paint in paints:
        comp = editor.EditorComponentAPIBus(bus.Broadcast, 'GetComponentOfType', paint, types[0]).GetValue()
        assert editor.EditorComponentAPIBus(bus.Broadcast, 'DisableComponents', [comp])
    result['disabled_paint_entities'] = len(paints)
    # The player's camera would overwrite diagnostic views each gameplay tick.
    camera_type = editor.EditorComponentAPIBus(bus.Broadcast, 'FindComponentTypeIdsByEntityType', ['Camera'], entity.EntityType().Game)[0]
    query = entity.SearchFilter(); query.components = {camera_type: {}}
    query_by_name = entity.SearchFilter(); query_by_name.names = ['Camera']
    static_camera = entity.SearchBus(bus.Broadcast, 'SearchEntities', query_by_name)[0]
    for camera_entity in entity.SearchBus(bus.Broadcast, 'SearchEntities', query):
        if camera_entity != static_camera:
            comp = editor.EditorComponentAPIBus(bus.Broadcast, 'GetComponentOfType', camera_entity, camera_type).GetValue()
            assert editor.EditorComponentAPIBus(bus.Broadcast, 'DisableComponents', [comp])
    general.set_viewport_expansion_policy('FixedSize'); general.set_viewport_size(960, 720)
    general.run_console('r_silpomAntiAliasing 4')
    general.idle_wait_frames(90)
    result['stage'] = 'enter gameplay'; flush()
    general.enter_game_mode()
    # Let the regular Editor event loop perform the scene transfer before
    # inspecting runtime entities; a startup-script polling loop delays it.
    QtCore.QTimer.singleShot(7000, capture_game)


def compare_game():
    global tower
    assert general.is_in_game_mode()
    runtime = general.find_game_entity('Irish Fieldstone Tower (Open Shaft)')
    assert runtime.IsValid()
    tower = runtime; ready()
    cases = [('outside', {'position': [593, 841, 3], 'rotation': [35, 0, 0]})]
    # Interior glancing view uses the established tower fixture pose.
    name, eye, direction, pose = next(camera_cases(json.loads(source)))
    angle = math.radians(pose['Rotate'][2]); c, s = math.cos(angle), math.sin(angle)
    cases.append(('inside', {'position': [pose['Translate'][0]+c*eye[0]-s*eye[1],
        pose['Translate'][1]+s*eye[0]+c*eye[1], pose['Translate'][2]+eye[2]],
        'rotation': [math.degrees(math.atan2(direction[2], math.hypot(*direction[:2]))), 0,
        (math.degrees(math.atan2(direction[1], direction[0]))-90+pose['Rotate'][2]+180)%360-180]}))
    for mode, label, raw in ((4, 'msaa2', 1), (4, 'msaa2-repair', 0), (1, 'smaa1', 1)):
        general.run_console(f'r_silpomMsaaRawCoverage {raw}')
        general.run_console(f'r_silpomAntiAliasing {mode}'); general.idle_wait_frames(90)
        result.setdefault('sample_states', {})[label] = str(general.get_cvar('r_multiSampleCount'))
        for name, pose in cases:
            result['stage'] = label+'/'+name; flush(); game_view(pose)
            screenshot(OUTPUT/(label+'-'+name+'-no-decals.png'))
            capture_hit(OUTPUT/(label+'-'+name+'-hits.dds'))
            capture_attachment(OUTPUT/(label+'-'+name+'-depth.dds'),
                ['Root','MainPipeline_0','OpaquePass','SilPOMHitForward'], 'DepthStencilInputOutput')
            capture_attachment(OUTPUT/(label+'-'+name+'-diffuse.dds'),
                ['Root','MainPipeline_0','OpaquePass','SilPOMHitForward'], 'DiffuseOutput')
    general.exit_game_mode()
    QtCore.QTimer.singleShot(3000, finish)


def capture_game():
    try:
        compare_game()
    except Exception:
        result['error'] = traceback.format_exc()
        finish()


def finish():
    result['saved_level_unchanged'] = LEVEL.read_bytes() == source
    result.update(passed='error' not in result and result['saved_level_unchanged'], stage='complete')
    flush()
    QtCore.QTimer.singleShot(0, general.exit_no_prompt)


try:
    flush(); main()
except Exception:
    result['error'] = traceback.format_exc()
    finish()

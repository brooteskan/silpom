"""PSO initialization regression for TG's existing planar-mesh and StoneWall fixtures.

Prepare with ordinary Python: material_initialization.py --prepare --project D:/TG/TGProject
Process the generated assets, then launch a separate Editor with --runpython <this file>.
SILPOM_MATERIAL_OUTPUT selects a directory under project/user. The test never saves
the level and exits the Editor. Warnings are recorded without suppressing them.
"""
import json
import math
import os
from pathlib import Path
import sys
import traceback


def prepare(project):
    output = project / 'Assets/SilPOMMaterialInitialization'
    output.mkdir(parents=True, exist_ok=True)
    sources = {
        'mesh_0': '../SilPOMPlanarFaces/material_0.material',
        'mesh_1': '../SilPOMPlanarFaces/material_1.material',
        'patch': '../SilPOM/StoneWall.material',
    }
    for name, parent in sources.items():
        source = json.loads((output / parent).read_text())
        for sided in (False, True):
            properties = {'general.doubleSided': sided}
            if name.startswith('mesh'):
                # Exercise image-asset copying as well as the inherited color/roughness.
                properties['baseColor.textureMap'] = '../Materials/StoneWall/stonewall_basecolor.png'
            document = {
                'materialType': source['materialType'],
                'materialTypeVersion': source['materialTypeVersion'],
                'parentMaterial': parent,
                'propertyValues': properties,
            }
            (output / f'{name}_{str(sided).lower()}.material').write_text(json.dumps(document, indent=2))
    print(output)


def run_editor():
    import azlmbr.asset as asset
    import azlmbr.atom as atom
    import azlmbr.bus as bus
    import azlmbr.components as components
    import azlmbr.debug as debug
    import azlmbr.editor as editor
    import azlmbr.entity as entity
    import azlmbr.legacy.general as general
    import azlmbr.math as azmath
    import azlmbr.paths as paths
    import azlmbr.silpom as silpom

    project = Path(paths.projectroot)
    output = project / 'user' / os.environ.get('SILPOM_MATERIAL_OUTPUT', 'SilPOMMaterialInitialization')
    output.mkdir(parents=True, exist_ok=True)
    result = {'passed': False, 'warnings': [], 'stages': [], 'captures': []}
    phase = 'startup'
    created = []
    original_viewport = general.get_viewport_size()
    original_policy = general.get_viewport_expansion_policy()
    original_background = general.get_cvar('ed_backgroundUpdatePeriod')

    def flush():
        (output / 'result.json').write_text(json.dumps(result, indent=2))

    def warning(args):
        message = str(args[-1])
        if 'Pipeline State Objects' in message:
            result['warnings'].append({'phase': phase, 'message': message})
        return False

    def asset_id(path):
        value = asset.AssetCatalogRequestBus(bus.Broadcast, 'GetAssetIdByPath', path, azmath.Uuid(), False)
        assert value.is_valid(), path
        return value

    def set_property(component, name, value):
        outcome = editor.EditorComponentAPIBus(bus.Broadcast, 'SetComponentProperty', component, name, value)
        assert outcome.IsSuccess(), str(outcome.GetError())

    def wait_ready(request, target):
        # Allow property edits to reach the controller before checking the previous generation.
        general.idle_wait_frames(5)
        for _ in range(900):
            if request(bus.Event, 'IsReady', target):
                general.idle_wait_frames(30)
                result['stages'].append({'phase': phase, 'status': request(bus.Event, 'GetStatus', target)})
                flush()
                return
            general.idle_wait_frames(1)
        raise RuntimeError(request(bus.Event, 'GetStatus', target))

    def capture(name, back=False):
        general.set_current_view_position(16.0, 19.5 if back else 12.5, 40.0)
        general.set_current_view_rotation(0.0, 0.0, 180.0 if back else 0.0)
        general.idle_wait_frames(60)
        filename = str(output / (name + '.png'))
        outcome = atom.FrameCaptureRequestBus(bus.Broadcast, 'CaptureScreenshot', filename)
        assert outcome.IsSuccess(), filename
        done = {}
        handler = atom.FrameCaptureNotificationBusHandler()
        handler.connect(outcome.GetValue())
        handler.add_callback('OnFrameCaptureFinished', lambda args: done.update(success=args[0] == atom.FrameCaptureResult_Success))
        try:
            for _ in range(300):
                if done:
                    break
                general.idle_wait_frames(1)
            assert done.get('success'), filename
        finally:
            handler.disconnect()
        result['captures'].append(filename)

    tracer = debug.TraceMessageBusHandler()
    tracer.connect(None)
    tracer.add_callback('OnPreWarning', warning)
    try:
        flush()
        general.idle_enable(True)
        general.run_console('ed_backgroundUpdatePeriod -1')
        general.run_console('r_meshInstancingEnabled false')
        phase = 'DefaultLevel.load'
        assert general.open_level('DefaultLevel')
        general.idle_wait_frames(180)
        fixture_filter = entity.SearchFilter()
        fixture_filter.names = ['SilPOM_Planar_FBX_Edge_Study']
        existing = entity.SearchBus(bus.Broadcast, 'SearchEntities', fixture_filter)
        assert len(existing) == 1, 'Expected one saved planar fixture for warning attribution'
        target = existing[0]
        mesh_type = editor.EditorComponentAPIBus(bus.Broadcast, 'FindComponentTypeIdsByEntityType', ['SilPOM Mesh'], entity.EntityType().Game)[0]
        component = editor.EditorComponentAPIBus(bus.Broadcast, 'GetComponentOfType', target, mesh_type).GetValue()
        wait_ready(silpom.SilPomMeshRequestBus, target)
        phase = 'DefaultLevel.fixture_disabled'
        assert editor.EditorComponentAPIBus(bus.Broadcast, 'DisableComponents', [component])
        general.idle_wait_frames(60)
        phase = 'DefaultLevel.fixture_reactivated'
        assert editor.EditorComponentAPIBus(bus.Broadcast, 'EnableComponents', [component])
        wait_ready(silpom.SilPomMeshRequestBus, target)
        editor.ToolsApplicationRequestBus(bus.Broadcast, 'DeleteEntityById', target)
        general.idle_wait_frames(30)
        general.set_viewport_expansion_policy('FixedSize')
        general.set_viewport_size(1280, 720)
        general.update_viewport()
        fixture = json.loads((project / 'Assets/SilPOMPlanarFaces/fixture.json').read_text())
        for kind in ('mesh', 'patch'):
            request = silpom.SilPomMeshRequestBus if kind == 'mesh' else silpom.SilPomPatchRequestBus
            for sided in ('false', 'true'):
                label = f'{kind}.{sided}'
                phase = label + '.initial'
                target = editor.ToolsApplicationRequestBus(bus.Broadcast, 'CreateNewEntity', entity.EntityId())
                created.append(target)
                editor.EditorEntityAPIBus(bus.Event, 'SetName', target, 'SilPOM Material Initialization ' + label)
                types = editor.EditorComponentAPIBus(bus.Broadcast, 'FindComponentTypeIdsByEntityType', ['SilPOM ' + kind.title()], entity.EntityType().Game)
                added = editor.EditorComponentAPIBus(bus.Broadcast, 'AddComponentsOfType', target, types)
                assert added.IsSuccess(), str(added.GetError())
                component = added.GetValue()[0]
                components.TransformBus(bus.Event, 'SetWorldTranslation', target, azmath.Vector3(16.0, 16.0, 40.0))
                prefix = 'Controller|Configuration|'
                if kind == 'mesh':
                    set_property(component, prefix + 'Mesh surface', asset_id('assets/silpomplanarfaces/planar_faces.fbx.silpommesh'))
                    for _ in range(300):
                        if 'discovered' in request(bus.Event, 'GetStatus', target):
                            break
                        general.idle_wait_frames(1)
                    properties = editor.EditorComponentAPIBus(bus.Broadcast, 'BuildComponentPropertyList', component)
                    scale_path = None
                    for path in properties:
                        if path.endswith('|Authored material ID'):
                            identity = editor.EditorComponentAPIBus(bus.Broadcast, 'GetComponentProperty', component, path).GetValue()
                            number = Path(fixture['materials'][identity]).stem.rsplit('_', 1)[1]
                            set_property(component, path.rsplit('|', 1)[0] + '|Material', asset_id(f'assets/silpommaterialinitialization/mesh_{number}_{sided}.azmaterial'))
                        elif path.endswith('|Height image'):
                            set_property(component, path, asset_id('assets/silpomplanarfaces/height.png.streamingimage'))
                        elif path.endswith('|Displacement (world metres)'):
                            scale_path = path
                            set_property(component, path, .3)
                        elif path.endswith('|UV offset'):
                            set_property(component, path, azmath.Vector2(.035, .02))
                    assert scale_path
                else:
                    components.TransformBus(bus.Event, 'SetWorldRotationQuaternion', target, azmath.Quaternion_CreateRotationX(math.pi / 2))
                    scale_path = prefix + 'Height scale (world metres)'
                    set_property(component, prefix + 'Material', asset_id(f'assets/silpommaterialinitialization/patch_{sided}.azmaterial'))
                wait_ready(request, target)
                editor.ToolsApplicationRequestBus(bus.Broadcast, 'SetSelectedEntities', [])
                capture(label + '.front')
                capture(label + '.back', back=True)
                phase = label + '.regenerated'
                set_property(component, scale_path, .2)
                wait_ready(request, target)
                phase = label + '.reactivated'
                assert editor.EditorComponentAPIBus(bus.Broadcast, 'DisableComponents', [component])
                general.idle_wait_frames(5)
                assert editor.EditorComponentAPIBus(bus.Broadcast, 'EnableComponents', [component])
                wait_ready(request, target)
                if kind == 'mesh':
                    phase = label + '.relief_shadow_edit'
                    set_property(component, prefix + 'Relief shadow steps', 0)
                    wait_ready(request, target)
                    capture(label + '.relief_off')
                editor.ToolsApplicationRequestBus(bus.Broadcast, 'DeleteEntityById', target)
                created.remove(target)
                general.idle_wait_frames(30)
        assert not result['warnings'], f"{len(result['warnings'])} runtime PSO warnings; see phases in result.json"
        result['passed'] = True
    except Exception:
        result['error'] = traceback.format_exc()
    finally:
        tracer.disconnect()
        for target in created:
            editor.ToolsApplicationRequestBus(bus.Broadcast, 'DeleteEntityById', target)
        general.set_viewport_size(int(original_viewport.x), int(original_viewport.y))
        general.set_viewport_expansion_policy(original_policy)
        general.run_console('ed_backgroundUpdatePeriod ' + str(original_background))
        flush()
        print(json.dumps(result, indent=2))
        general.exit_no_prompt()


if '--prepare' in sys.argv:
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--prepare', action='store_true')
    parser.add_argument('--project', type=Path, required=True)
    prepare(parser.parse_args().project)
else:
    run_editor()

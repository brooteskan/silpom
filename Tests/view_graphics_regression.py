"""Open the disposable planar fixture and leave the SilPOM regression visible."""

import json
import math
from pathlib import Path

import azlmbr.asset as asset
import azlmbr.bus as bus
import azlmbr.components as components
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.math as azmath
import azlmbr.paths as paths
import azlmbr.silpom as silpom


LEVEL_NAME = "SilPOMPlanarAB_20260920_TexelReuse"
MANIFEST_PATH = Path(paths.projectroot) / "Assets" / "SilPOMPlanarAB" / "manifest.json"


def set_property(component_id, name, value):
    outcome = editor.EditorComponentAPIBus(
        bus.Broadcast,
        "SetComponentProperty",
        component_id,
        "Controller|Configuration|" + name,
        value,
    )
    if not outcome.IsSuccess():
        raise RuntimeError("Could not set SilPOM property '{}': {}".format(name, outcome.GetError()))


def wait_for_asset(asset_path, frame_limit=600):
    asset_id = asset.AssetCatalogRequestBus(
        bus.Broadcast, "GetAssetIdByPath", asset_path, azmath.Uuid(), False
    )
    for _ in range(frame_limit):
        if asset_id.is_valid():
            return asset_id
        general.idle_wait_frames(1)
        asset_id = asset.AssetCatalogRequestBus(
            bus.Broadcast, "GetAssetIdByPath", asset_path, azmath.Uuid(), False
        )
    raise RuntimeError("Timed out waiting for asset: {}".format(asset_path))


with MANIFEST_PATH.open("r", encoding="utf-8") as manifest_file:
    manifest = json.load(manifest_file)

general.idle_enable(True)
general.open_level(LEVEL_NAME)
general.set_viewport_expansion_policy("FixedSize")
general.set_viewport_size(*manifest["resolution"])
general.run_console("r_renderScale 1.0")
general.run_console("vsync_interval 0")

patch_entity = editor.ToolsApplicationRequestBus(
    bus.Broadcast, "CreateNewEntity", entity.EntityId()
)
editor.EditorEntityAPIBus(bus.Event, "SetName", patch_entity, "SilPOM Graphics Regression")

component_type = editor.EditorComponentAPIBus(
    bus.Broadcast,
    "FindComponentTypeIdsByEntityType",
    ["SilPOM Patch"],
    entity.EntityType().Game,
)[0]
add_result = editor.EditorComponentAPIBus(
    bus.Broadcast, "AddComponentsOfType", patch_entity, [component_type]
)
if not add_result.IsSuccess():
    raise RuntimeError("Could not add the SilPOM Patch component: {}".format(add_result.GetError()))
component_id = add_result.GetValue()[0]

material_id = wait_for_asset(manifest["quad_optimized_material"])

set_property(component_id, "Width", manifest["width_metres"])
set_property(component_id, "Height", manifest["height_metres"])
set_property(component_id, "Height scale (world metres)", manifest["scale_metres"])
set_property(component_id, "Reference height", manifest["midpoint"])
set_property(component_id, "Maximum cells", manifest["max_cells"])
set_property(component_id, "Reuse adjacent height texels", True)
set_property(component_id, "Material", material_id)

components.TransformBus(
    bus.Event, "SetWorldTranslation", patch_entity, azmath.Vector3(16.0, 16.0, 40.0)
)
components.TransformBus(
    bus.Event,
    "SetWorldRotationQuaternion",
    patch_entity,
    azmath.Quaternion_CreateRotationX(math.pi * 0.5),
)

camera_angle = 85.0
camera_distance = 3.0
camera_position = azmath.Vector3(
    16.0 + camera_distance * math.sin(math.radians(camera_angle)),
    16.0 - camera_distance * math.cos(math.radians(camera_angle)),
    40.0,
)
general.set_current_view_position(camera_position.x, camera_position.y, camera_position.z)
general.set_current_view_rotation(0.0, 0.0, camera_angle)

editor.ToolsApplicationRequestBus(bus.Broadcast, "SetSelectedEntities", [patch_entity])
silpom.SilPomPlanarBenchmarkBus(
    bus.Broadcast,
    "SetOverlay",
    "SilPOM graphics regression | 85 deg grazing view | Optimized bounds path ON\n"
    "Select the patch and toggle 'Reuse adjacent height texels' OFF to compare the baseline.",
)
general.idle_wait_frames(30)
print("SilPOM graphics regression view is ready; leaving Editor open.")

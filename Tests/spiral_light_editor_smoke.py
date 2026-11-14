"""Test spiral modes on the existing tower; exit without saving any level."""
import hashlib
import json
from pathlib import Path
import time
import traceback

import azlmbr.bus as bus
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.math as azmath
import azlmbr.paths as paths
import azlmbr.silpom as silpom

project = Path(paths.projectroot)
level = project / "Levels/VueLiveLink/VueLiveLink.prefab"
package = project / "Assets/SilPOMPlanarAB/IrishFieldstoneTowerOpenShaft"
prefab = package / "IrishFieldstoneTowerOpenShaft.prefab"
output = project / "user/SilPOMSpiralSmoke/result.json"
output.parent.mkdir(parents=True, exist_ok=True)
result = {"passed": False, "cases": []}
original_mode = None
original_path = None
component = None


def flush():
    output.write_text(json.dumps(result, indent=2), encoding="utf-8")


def ready():
    general.idle_wait_frames(3)
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        if silpom.SilPomMeshRequestBus(bus.Event, "IsReady", tower):
            return silpom.SilPomMeshRequestBus(bus.Event, "GetStatus", tower)
        general.idle_wait_frames(1)
    raise RuntimeError(silpom.SilPomMeshRequestBus(bus.Event, "GetStatus", tower))


def get_value(path):
    value = editor.EditorComponentAPIBus(bus.Broadcast, "GetComponentProperty", component, path)
    assert value.IsSuccess(), path
    return value.GetValue()


def set_value(path, value):
    changed = editor.EditorComponentAPIBus(bus.Broadcast, "SetComponentProperty", component, path, value)
    assert changed.IsSuccess(), str(changed.GetError())


try:
    hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (level, prefab)}
    result["source_hashes"] = hashes
    configuration = json.loads(prefab.read_bytes())["Entities"]["TowerMesh"]["Components"]["EditorMeshComponent"]["Controller"]["Configuration"]
    sidecar = json.loads((package / "tower-open-shaft-no-landings-silpom.silpom.json").read_bytes())
    ordinary_profiles = {p["ProfileId"] for p in configuration["Profiles"] if p.get("UseBaseMesh", False)}
    displaced = {(f["material_id"], f["profile"]) for f in sidecar["faces"] if f["region"] and f["profile"] not in ordinary_profiles}
    opted_in = {m["MaterialIdentity"] for m in configuration["Materials"] if m.get("ReceiveSpiralLight", False)}
    expected_all = len(displaced)
    expected_selected = sum(identity in opted_in for identity, profile in displaced)
    result["expected_all_batches"] = expected_all
    result["expected_selected_batches"] = expected_selected
    flush()
    general.idle_enable(True)
    if general.get_current_level_name().lower() != "vuelivelink":
        assert general.open_level(str(level))
    general.idle_wait_frames(60)
    query = entity.SearchFilter()
    query.names = ["Irish Fieldstone Tower (Open Shaft)"]
    towers = entity.SearchBus(bus.Broadcast, "SearchEntities", query)
    assert len(towers) == 1
    tower = towers[0]
    ready()
    types = editor.EditorComponentAPIBus(bus.Broadcast, "FindComponentTypeIdsByEntityType", ["SilPOM Mesh"], entity.EntityType().Game)
    found = editor.EditorComponentAPIBus(bus.Broadcast, "GetComponentOfType", tower, types[0])
    assert found.IsSuccess()
    component = found.GetValue()
    properties = editor.EditorComponentAPIBus(bus.Broadcast, "BuildComponentPropertyList", component)
    mode_path = next(p for p in properties if p.endswith("|Spiral light"))
    path_path = next(p for p in properties if p.endswith("|Spiral light path"))
    original_mode = get_value(mode_path)
    original_path = get_value(path_path)
    result["original_mode"] = str(original_mode)
    for mode, expected in [(0, "spiral light off"), (2, f"spiral light on {expected_all} relief batches"),
                           (1, f"spiral light on {expected_selected} relief batches")]:
        set_value(mode_path, mode)
        status = ready()
        assert expected in status, status
        general.idle_wait_frames(20)
        result["cases"].append({"mode": mode, "status": status})
        flush()
    # Off must not require a valid path even while additional windows are stored.
    set_value(mode_path, 0)
    set_value(path_path, azmath.Vector4(0.0, 0.0, 0.0, 0.0))
    assert "spiral light off" in ready()
    result["off_ignores_unused_path"] = True
    set_value(path_path, original_path)
    set_value(mode_path, original_mode)
    ready()
    result["restored"] = True
    assert all(hashlib.sha256(Path(p).read_bytes()).hexdigest() == h for p, h in hashes.items())
    result["level_and_prefab_unchanged"] = True
    result["passed"] = True
except Exception:
    result["error"] = traceback.format_exc()
finally:
    if component is not None and original_mode is not None and not result.get("restored"):
        try:
            if original_path is not None:
                set_value(path_path, original_path)
            set_value(mode_path, original_mode)
            ready()
            result["restored"] = True
        except Exception:
            result["restore_error"] = traceback.format_exc()
    flush()
    print(json.dumps(result, indent=2))
    general.exit_no_prompt()

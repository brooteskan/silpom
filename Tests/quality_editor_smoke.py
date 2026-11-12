"""Check level quality controls in a temporary Editor session; never save."""
import hashlib
import json
import math
from pathlib import Path
import time
import traceback

import azlmbr.bus as bus
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.paths as paths
import azlmbr.silpom as silpom

project = Path(paths.projectroot)
output = project / "user/SilPOMQualitySmoke/result.json"
output.parent.mkdir(parents=True, exist_ok=True)
level = project / "Levels/VueLiveLink/VueLiveLink.prefab"
result = {"passed": False, "cases": []}
originals = {}
component = None


def flush():
    output.write_text(json.dumps(result, indent=2), encoding="utf-8")


def ready():
    deadline = time.monotonic() + 90
    general.idle_wait_frames(3)
    while time.monotonic() < deadline:
        if silpom.SilPomMeshRequestBus(bus.Event, "IsReady", tower):
            return
        general.idle_wait_frames(1)
    raise RuntimeError(silpom.SilPomMeshRequestBus(bus.Event, "GetStatus", tower))


def set_value(label, value):
    changed = editor.EditorComponentAPIBus(bus.Broadcast, "SetComponentProperty", component, properties[label], value)
    assert changed.IsSuccess(), str(changed.GetError())
    actual = editor.EditorComponentAPIBus(bus.Broadcast, "GetComponentProperty", component, properties[label])
    assert actual.IsSuccess(), label
    stored = actual.GetValue()
    matches = math.isclose(stored, value, rel_tol=1e-6, abs_tol=1e-6) if isinstance(value, float) else stored == value
    assert matches, f"{label}: stored {stored}, expected {value}"


try:
    result["level_sha256"] = hashlib.sha256(level.read_bytes()).hexdigest()
    flush()
    general.idle_enable(True)
    if general.get_current_level_name().lower() != "vuelivelink":
        assert general.open_level(str(level))
    general.idle_wait_frames(90)
    query = entity.SearchFilter()
    query.names = ["Irish Fieldstone Tower (Open Shaft)"]
    towers = entity.SearchBus(bus.Broadcast, "SearchEntities", query)
    assert len(towers) == 1
    tower = towers[0]
    ready()
    types = editor.EditorComponentAPIBus(bus.Broadcast, "FindComponentTypeIdsByEntityType",
                                        ["SilPOM Mesh"], entity.EntityType().Game)
    found = editor.EditorComponentAPIBus(bus.Broadcast, "GetComponentOfType", tower, types[0])
    assert found.IsSuccess()
    component = found.GetValue()
    paths_available = editor.EditorComponentAPIBus(bus.Broadcast, "BuildComponentPropertyList", component)
    labels = ["Relief LOD pixel budget (higher = cheaper)", "Maximum relief distance (closer = cheaper)",
              "Validation distance fade start", "Validation distance fade end (0 = off)",
              "Distant validation iterations (lower = cheaper)", "Distant validation retry cap (lower = cheaper)",
              "Distant validation tolerance (higher may be cheaper)", "Spiral-light shadow samples (lower = cheaper)",
              "Height-derived normals (off = cheaper)"]
    properties = {label: next(p for p in paths_available if p.endswith("|" + label)) for label in labels}
    for label in labels:
        value = editor.EditorComponentAPIBus(bus.Broadcast, "GetComponentProperty", component, properties[label])
        assert value.IsSuccess()
        originals[label] = value.GetValue()
    result["original_settings"] = originals.copy()
    # Retain any user tuning of the original near validation controls.
    for values in [(8.0, 3.0, 5.0, 15.0, 4, 0, 10.0, 0, False),
                   (4.0, 0.05, 0.0, 0.0, 4, 1, 1.0, 18, True)]:
        for label, value in zip(labels, values):
            set_value(label, value)
        ready()
        general.idle_wait_frames(30)
        result["cases"].append({"settings": dict(zip(labels, values)),
                                "status": silpom.SilPomMeshRequestBus(bus.Event, "GetStatus", tower)})
        flush()
    # Restore an arbitrary saved fade interval without transient invalid bounds.
    set_value(labels[3], 0.0)
    for label, value in originals.items():
        set_value(label, value)
    ready()
    result["restored"] = True
    assert hashlib.sha256(level.read_bytes()).hexdigest() == result["level_sha256"], "Level file changed"
    result["passed"] = True
except Exception:
    result["error"] = traceback.format_exc()
finally:
    if component is not None and originals and not result.get("restored"):
        try:
            set_value("Validation distance fade end (0 = off)", 0.0)
            for label, value in originals.items():
                set_value(label, value)
            ready()
            result["restored"] = True
        except Exception:
            result["restore_error"] = traceback.format_exc()
    flush()
    print(json.dumps(result, indent=2))
    general.exit_no_prompt()

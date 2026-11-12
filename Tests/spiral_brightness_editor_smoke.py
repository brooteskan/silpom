"""Verify master spiral gain and top fade; never save the level."""
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
import azlmbr.math as azmath
import azlmbr.paths as paths
import azlmbr.silpom as silpom

project = Path(paths.projectroot)
level = project / "Levels/VueLiveLink/VueLiveLink.prefab"
prefab = project / "Assets/SilPOMPlanarAB/IrishFieldstoneTowerOpenShaft/IrishFieldstoneTowerOpenShaft.prefab"
output = project / "user/SilPOMSpiralBrightnessSmoke/result.json"
output.parent.mkdir(parents=True, exist_ok=True)
result = {"passed": False, "cases": []}
originals = {}
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
    outcome = editor.EditorComponentAPIBus(bus.Broadcast, "GetComponentProperty", component, path)
    assert outcome.IsSuccess(), path
    return outcome.GetValue()


def set_value(path, value):
    outcome = editor.EditorComponentAPIBus(bus.Broadcast, "SetComponentProperty", component, path, value)
    assert outcome.IsSuccess(), str(outcome.GetError())


try:
    hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (level, prefab)}
    result["source_hashes"] = hashes
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
    props = editor.EditorComponentAPIBus(bus.Broadcast, "BuildComponentPropertyList", component)
    mode_path = next(p for p in props if p.endswith("|Spiral light"))
    gain_path = next(p for p in props if p.endswith("|Spiral light master brightness"))
    end_path = next(p for p in props if p.endswith("|Spiral light top fade"))
    for p in (mode_path, gain_path, end_path):
        originals[p] = get_value(p)
    end = originals[end_path]
    assert math.isclose(end.x, 24.0, abs_tol=1e-5) and math.isclose(end.y, 2.0, abs_tol=1e-5)
    result["top_fade"] = [end.x, end.y]
    set_value(mode_path, 2)
    for brightness in (0.0, 0.5, 1.0, 2.0):
        set_value(gain_path, brightness)
        assert math.isclose(get_value(gain_path), brightness, abs_tol=1e-6)
        status = ready()
        assert ("spiral light muted" if brightness == 0 else "spiral light on 14 relief batches") in status, status
        general.idle_wait_frames(20)
        result["cases"].append({"brightness": brightness, "status": status})
        flush()
    set_value(end_path, azmath.Vector2(24.0, 4.0))
    ready()
    assert math.isclose(get_value(end_path).y, 4.0, abs_tol=1e-6)
    result["fade_editable"] = True
    for p, value in originals.items():
        set_value(p, value)
    ready()
    result["restored"] = True
    assert all(hashlib.sha256(Path(p).read_bytes()).hexdigest() == h for p, h in hashes.items())
    result["level_and_prefab_unchanged_by_test"] = True
    result["passed"] = True
except Exception:
    result["error"] = traceback.format_exc()
finally:
    if component is not None and originals and not result.get("restored"):
        try:
            for p, value in originals.items():
                set_value(p, value)
            ready()
            result["restored"] = True
        except Exception:
            result["restore_error"] = traceback.format_exc()
    flush()
    print(json.dumps(result, indent=2))
    general.exit_no_prompt()

"""Create a disposable benchmark level snapshot; never modifies DefaultLevel."""
import argparse
import hashlib
import json
from pathlib import Path


def prepare(project, name):
    if not name.startswith("SilPOMPlanarAB_") or not all(c.isalnum() or c == "_" for c in name):
        raise ValueError("Use a unique SilPOMPlanarAB_<name> level name")
    source = project / "Levels/DefaultLevel/DefaultLevel.prefab"
    original = source.read_bytes()
    document = json.loads(original)
    # This copy measures one patch. Keep other scene content, but omit existing
    # SilPOM entities from the copy (the user's source level is never written).
    removed = {key: value["Name"] for key, value in document["Entities"].items()
               if any(component.get("$type") == "EditorPatchComponent"
                      for component in value.get("Components", {}).values())}
    for key in removed:
        del document["Entities"][key]
    for value in [document["ContainerEntity"], *document["Entities"].values()]:
        for component in value.get("Components", {}).values():
            if "Child Entity Order" in component:
                component["Child Entity Order"] = [key for key in component["Child Entity Order"] if key not in removed]
            if component.get("Parent Entity") in removed:
                raise ValueError("Existing SilPOM entity has children; snapshot needs explicit handling")
            if "LocalBookmarkFileName" in component:
                component["LocalBookmarkFileName"] = name + ".setreg"
    folder = project / "Levels" / name
    folder.mkdir(parents=True, exist_ok=False)
    target = folder / (name + ".prefab")
    target.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    metadata = {"source": str(source), "source_sha256": hashlib.sha256(original).hexdigest(),
                "snapshot": str(target), "snapshot_sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
                "omitted_from_copy_only": removed}
    (folder / "snapshot.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    assert source.read_bytes() == original, "Source level changed concurrently; regenerate snapshot"
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--name", required=True)
    args = parser.parse_args()
    prepare(args.project.resolve(), args.name)

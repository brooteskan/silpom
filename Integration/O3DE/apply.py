#!/usr/bin/env python3
"""Apply the frozen TG engine integration without resetting local repositories."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


BUNDLE = Path(__file__).resolve().parent


def normalized(data):
    return data.replace(b"\r\n", b"\n")


def digest(data):
    return hashlib.sha256(normalized(data)).hexdigest()


def safe_path(root, relative):
    path = (root / relative).resolve()
    if not path.is_relative_to(root) or path == root:
        raise ValueError(f"Path escapes its root: {relative}")
    return path


def file_hash(path):
    return digest(path.read_bytes()) if path.is_file() else None


def git(root, *arguments):
    result = subprocess.run(["git", "-C", str(root), *arguments],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise ValueError(result.stderr.decode(errors="replace").strip())
    return result.stdout.decode(errors="replace").strip()


def patch_sections(patch):
    sections = {}
    for section in re.split(rb"(?=^diff --git )", patch, flags=re.MULTILINE):
        if not section:
            continue
        header = section.split(b"\n", 1)[0].decode()
        match = re.fullmatch(r"diff --git a/(.+) b/(.+)", header)
        if not match or match[1] != match[2] or match[1] in sections:
            raise ValueError(f"Unsupported or duplicate patch header: {header}")
        sections[match[1]] = section
    return sections


def prepare(engine, project, verify_only=False, experimental_paint_bake=False):
    manifest = json.loads((BUNDLE / "manifest.json").read_text(encoding="utf-8"))
    if manifest["schema_version"] != 1:
        raise ValueError("Unsupported manifest schema")
    if json.loads((engine / "engine.json").read_text(encoding="utf-8-sig")).get("sdk_engine"):
        raise ValueError("An installed SDK cannot be patched in place. Select an O3DE source checkout.")
    plans = []
    for component in manifest["components"]:
        root = engine if component["name"] == "engine" else safe_path(project, component["project_path"])
        if not root.is_dir():
            raise ValueError(f"Missing source checkout: {root}")
        if not verify_only:
            head = git(root, "rev-parse", "HEAD")
            if head != component["upstream_commit"]:
                raise ValueError(f"{component['name']}: expected HEAD {component['upstream_commit']}, found {head}")
            if Path(git(root, "rev-parse", "--show-toplevel")).resolve() != root:
                raise ValueError(f"{root} must be its own Git checkout")
        patch = normalized(safe_path(BUNDLE, component["patch"]).read_bytes())
        if digest(patch) != component["patch_sha256"]:
            raise ValueError(f"Patch integrity check failed: {component['patch']}")
        sections = patch_sections(patch)
        if set(sections) != {entry["path"] for entry in component["files"]}:
            raise ValueError(f"Patch and file manifest disagree: {component['name']}")
        if experimental_paint_bake and component["name"] == "engine":
            experiment = manifest["experimental_paint_bake"]
            contents = normalized(safe_path(BUNDLE, experiment["patch"]).read_bytes())
            if digest(contents) != experiment["patch_sha256"]:
                raise ValueError("Experimental paint-bake patch integrity check failed")
            replacement = patch_sections(contents)
            entry = experiment["file"]
            if set(replacement) != {entry["path"]} or entry["path"] not in sections:
                raise ValueError("Invalid experimental paint-bake replacement")
            sections.update(replacement)
            component = dict(component, files=[entry if old["path"] == entry["path"] else old for old in component["files"]])
        pending = []
        selected = []
        for entry in component["files"]:
            upgrades = {}
            for upgrade in entry.get("upgrade_from", []):
                contents = normalized(safe_path(BUNDLE, upgrade["patch"]).read_bytes())
                if digest(contents) != upgrade["patch_sha256"]:
                    raise ValueError(f"Patch integrity check failed: {upgrade['patch']}")
                replacement = patch_sections(contents)
                if set(replacement) != {entry["path"]} or upgrade["before_sha256"] in upgrades:
                    raise ValueError(f"Invalid or duplicate upgrade: {upgrade['patch']}")
                upgrades[upgrade["before_sha256"]] = replacement[entry["path"]]
            path = safe_path(root, entry["path"])
            if path.exists() and not path.is_file():
                raise ValueError(f"Expected a file, found another object: {path}")
            actual = file_hash(path)
            if actual == entry["after_sha256"]:
                continue
            if verify_only or (actual != entry["before_sha256"] and actual not in upgrades):
                raise ValueError(f"Conflicting or incomplete file: {path}. Preserve local work and use the pinned source checkout.")
            pending.append(entry["path"])
            # Added-file aggregate sections cannot replace an installed older
            # file. A known hash selects a separately guarded modification patch.
            selected.append(sections[entry["path"]] if actual == entry["before_sha256"] else upgrades[actual])
        plans.append((component, root, b"".join(selected), len(pending)))
    return plans


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--experimental-paint-bake", action="store_true",
                        help="Opt into the rejected color/normal bake prototype for benchmarks; not a production optimization")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check-only", action="store_true", help="Preflight every component without modifying source")
    mode.add_argument("--verify-only", action="store_true", help="Require all patched contents; also works on source copies without Git metadata")
    args = parser.parse_args(argv)
    engine, project = args.engine.resolve(), args.project.resolve()
    plans = prepare(engine, project, args.verify_only, args.experimental_paint_bake)
    if args.verify_only:
        print("Verified all engine and terrain integration file hashes.")
        return 0
    # Validate all components before mutating any component. Each complete file
    # already at its final hash is skipped, supporting older partially applied sets.
    with tempfile.TemporaryDirectory(prefix="tg-o3de-patch-") as temporary:
        staged = []
        for component, root, patch, count in plans:
            print(f"{component['name']}: {count} files pending; {len(component['files']) - count} already match.")
            if patch:
                patch_path = Path(temporary) / component["patch"]
                patch_path.write_bytes(patch)
                git(root, "apply", "--check", "--whitespace=nowarn", str(patch_path))
                staged.append((root, patch_path))
        if args.check_only:
            print("Preflight passed. No source files changed.")
            return 0
        for root, patch_path in staged:
            git(root, "apply", "--whitespace=nowarn", str(patch_path))
    prepare(engine, project, verify_only=True, experimental_paint_bake=args.experimental_paint_bake)
    print("Applied and verified the complete TG engine integration. Rebuild native modules and shader assets.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, KeyError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)

#!/usr/bin/env python3
"""Exercise patch installation on isolated sparse clones of the pinned sources."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

BUNDLE = Path(__file__).resolve().parent


def run(arguments, success=True):
    result = subprocess.run([str(a) for a in arguments], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=dict(os.environ, GIT_OPTIONAL_LOCKS="0"))
    output = result.stdout.decode(errors="replace")
    if (result.returncode == 0) != success:
        raise AssertionError(f"Unexpected exit {result.returncode}: {arguments}\n{output}")
    return output


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--terrain", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        raise ValueError("Choose a new output directory; validation never deletes existing data.")
    output.mkdir(parents=True)
    manifest = json.loads((BUNDLE/"manifest.json").read_text())
    results = []
    inputs = [args.baseline.resolve(), args.terrain.resolve()]
    before_status = [run(["git", "-C", path, "status", "--porcelain", "--untracked-files=all"]) for path in inputs]
    for crlf in (False, True):
        label = "crlf" if crlf else "lf"
        fixture = output/label
        project, engine = fixture/"TGProject", fixture/"engine"
        roots = [engine, project/"Gems/terrain-compositor"]
        for source, root, component in zip(inputs, roots, manifest["components"]):
            root.parent.mkdir(parents=True, exist_ok=True)
            run(["git", "clone", "--shared", "--no-checkout", source, root])
            run(["git", "-C", root, "config", "core.autocrlf", "true" if crlf else "false"])
            run(["git", "-C", root, "sparse-checkout", "init", "--no-cone"])
            patterns = ["/"+entry["path"] for entry in component["files"] if entry["before_sha256"]]
            (root/".git/info/sparse-checkout").write_text("\n".join(patterns)+"\n", encoding="utf-8")
            run(["git", "-C", root, "checkout", "--detach", component["upstream_commit"]])
        (project/"project.json").write_text('{"project_name":"TGProject"}\n')
        (project/"user").mkdir()
        user = project/"user/project.json"
        user.write_text('{"engine_path":"unchanged-sdk","other_key":42}\n')
        tracked = [root/entry["path"] for root,component in zip(roots,manifest["components"]) for entry in component["files"]]
        def snapshot():
            return {str(path):sha(path) for path in tracked+[user]}
        def apply(*arguments, success=True, bundle=BUNDLE):
            return run([sys.executable,bundle/"apply.py","--engine",engine,"--project",project,*arguments], success)
        original = snapshot()
        apply("--check-only")
        assert snapshot() == original, "Check-only changed source"
        apply("--verify-only", success=False)
        if crlf:
            assert b"\r\n" in (engine/"engine.json").read_bytes(), "CRLF fixture was not checked out with CRLF"
        results.append(label+": read-only preflight and pristine verification refusal")
        # A terrain conflict must prevent even the otherwise-clean engine patch.
        conflict = roots[1]/manifest["components"][1]["files"][0]["path"]
        content = conflict.read_bytes()
        conflict.write_bytes(content+b"\n// local conflict\n")
        conflict_snapshot = snapshot()
        apply(success=False)
        assert snapshot() == conflict_snapshot, "Cross-component conflict mutated source"
        conflict.write_bytes(content)
        results.append(label+": cross-component conflict refused without mutation")
        if not crlf:
            plan = run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        BUNDLE/"Build-ProfilingEngine.ps1", "-EnginePath",engine,"-ProjectPath",project,
                        "-BuildPath",fixture/"planned-build","-Python",sys.executable,"-ShaderModel","6_2","-PlanOnly"])
            assert "TG_DX12_SHADER_MODEL=6_2" in plan and "TGProject.GameLauncher" in plan
            assert not (fixture/"planned-build").exists() and snapshot()==original, "PlanOnly modified files"
            (output/"powershell-plan.txt").write_text(plan,encoding="utf-8")
            results.append("PowerShell SM 6.2 build/asset plan; no file or engine-selection changes")
        log = apply()
        apply("--verify-only")
        applied = snapshot()
        apply()
        assert snapshot() == applied, "Reapplication changed files"
        results.append(label+": complete application, all 95 output hashes, idempotent reapplication")
        (output/(label+"-apply.txt")).write_text(log,encoding="utf-8")
        conflict = engine/manifest["components"][0]["files"][0]["path"]
        content = conflict.read_bytes()
        conflict.write_bytes(content+b"\n// preserve this edit\n")
        changed = snapshot()
        apply(success=False)
        assert snapshot()==changed, "Conflicting local edit was changed"
        conflict.write_bytes(content)
        results.append(label+": post-install local edit refused and preserved")
        # A corrupted patch must fail before any source mutation.
        damaged = fixture/"damaged-bundle"
        damaged.mkdir()
        for name in ("apply.py","manifest.json","engine.patch","terrain-compositor.patch"):
            shutil.copyfile(BUNDLE/name,damaged/name)
        with (damaged/"engine.patch").open("ab") as stream:
            stream.write(b"\ncorruption\n")
        apply("--check-only",success=False,bundle=damaged)
        assert snapshot()==applied
        results.append(label+": patch integrity failure refused without mutation")
    after_status = [run(["git", "-C", path, "status", "--porcelain", "--untracked-files=all"]) for path in inputs]
    assert before_status == after_status, "Original source checkout status changed"
    report = {"bundle":manifest["bundle"],"engine_base":manifest["components"][0]["upstream_commit"],
              "engine_files":91,"terrain_integration_files":4,"passed":True,"checks":results,
              "limitations":["No O3DE rebuild performed by packaging validation.",
                             "Original visual reference used DX12 SM 6.7.",
                             "Full tower SM 6.2 and Ryzen GPU profiling must be validated on the other PC."]}
    (output/"VALIDATION.json").write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(report,indent=2))


if __name__ == "__main__":
    main()

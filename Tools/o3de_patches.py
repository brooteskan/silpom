"""Locate the pinned brooteskan/o3de-patches checkout that SilPOM builds against."""
import hashlib
import json
import os
from pathlib import Path

GEM = Path(__file__).resolve().parents[1]
PIN_PATH = GEM / "Integration/O3DEPatches.json"


def pin():
    return json.loads(PIN_PATH.read_text(encoding="utf-8"))


def _digest(path):
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def candidates():
    configured = os.environ.get("SILPOM_O3DE_PATCHES")
    if configured:
        yield Path(configured)
    yield GEM.parent / "o3de-patches"
    yield GEM.parent.parent / "o3de-patches"


def check(root):
    """Return None if root holds the pinned bundle, otherwise the reason it does not."""
    expected = pin()
    manifest_path = root / "manifest.json"
    if not manifest_path.is_file():
        return f"{root}: no manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("bundle") != expected["bundle"]:
        return f"{root}: bundle {manifest.get('bundle')}, pinned {expected['bundle']}"
    if _digest(root / "engine.patch") != expected["engine_patch_sha256"]:
        return f"{root}: engine.patch differs from the pinned hash"
    return None


def locate(required=False):
    """Return the pinned checkout, or None (raise if required) when none is found."""
    reasons = []
    for root in candidates():
        root = root.resolve()
        if not root.is_dir():
            continue
        reason = check(root)
        if reason is None:
            return root
        reasons.append(reason)
    if required:
        detail = "; ".join(reasons) or "no checkout found"
        raise FileNotFoundError(f"Pinned o3de-patches checkout required ({detail}). "
                                f"Set SILPOM_O3DE_PATCHES; see Integration/README.md.")
    return None


def evidence():
    """Bundle identity for benchmark records, whether or not a checkout is present."""
    expected = pin()
    root = locate()
    return {"repository": expected["repository"], "pinned_commit": expected["commit"],
            "bundle": expected["bundle"], "engine_patch_sha256": expected["engine_patch_sha256"],
            "checkout": str(root) if root else None,
            "manifest_sha256": _digest(root / "manifest.json") if root else None}

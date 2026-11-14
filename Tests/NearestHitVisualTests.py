"""Check a fixed-camera capture directory against the approved visual reference.

Usage: python NearestHitVisualTests.py <capture-root-with-results.json>
The Editor capture helper must produce the PNG and hit DDS files in current/.
"""
import hashlib
import json
from pathlib import Path
import struct
import sys

import numpy as np
from PIL import Image


def capture_hash(path, kind):
    if kind == "rgba":
        return hashlib.sha256(Image.open(path).convert("RGBA").tobytes()).hexdigest()
    raw = path.read_bytes()
    assert raw[:4] == b"DDS " and raw[84:88] == b"DX10", path
    assert struct.unpack_from("<I", raw, 128)[0] == 3, "Expected RGBA32_UINT hit records"
    height, width = struct.unpack_from("<II", raw, 12)
    values = np.frombuffer(raw, dtype="<u4", offset=148, count=width * height * 4).copy()
    values = values.reshape(height, width, 4)
    # Instance ownership allocation can change between Editor launches. The UV,
    # depth, representation flags and hit status must remain bit-identical.
    values[:, :, 3] &= 3
    return hashlib.sha256(values.tobytes()).hexdigest()


def compare(root):
    manifest = json.loads((Path(__file__).parent / "Reference/NearestHitVisual.json").read_text())
    result = json.loads((root / "results.json").read_text())
    assert result["passed"] and result["restored"], "Capture did not complete and restore its settings"
    # The level hash records provenance. Unrelated entities can be edited while
    # optimizing the tower; require its quality controls and rendered output.
    assert result["quality_controls"] == manifest["tower_quality_controls"], "Tower settings differ from the visual reference"
    cameras = result["variants"]["current"]["views"]
    for name, expected in manifest["cameras"].items():
        for key, value in expected.items():
            assert cameras[name][key] == value, (name, key, "Camera differs")
    changed = []
    for name, expected in manifest["captures"].items():
        if capture_hash(root / "current" / name, expected["kind"]) != expected["sha256"]:
            changed.append(name)
    assert not changed, "Visual reference changed: " + ", ".join(changed)
    print(f"{len(manifest['captures'])} static/motion images and hit buffers match the visual reference exactly.")


if __name__ == "__main__":
    compare(Path(sys.argv[1]))

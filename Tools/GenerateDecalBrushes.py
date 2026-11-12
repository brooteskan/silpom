"""Rebuild the small, deterministic decal brush assets (standard library only)."""
import json
import math
from pathlib import Path
import struct
import zlib

destination = Path(__file__).resolve().parents[1] / "Assets/Materials/DecalPaint"
destination.mkdir(parents=True, exist_ok=True)


def png(path, pixels, size=128):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    raw = b"".join(b"\0" + pixels[y * size * 4:(y + 1) * size * 4] for y in range(size))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", size, size, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


for name in ("soft_round", "hard_round"):
    values = bytearray()
    for y in range(128):
        for x in range(128):
            radius = math.hypot((x + .5 - 64) / 63, (y + .5 - 64) / 63)
            t = max(0, min(1, (1 - radius) / (1 if name == "soft_round" else .04)))
            alpha = round(255 * t * t * (3 - 2 * t))
            values.extend((255, 255, 255, alpha))
    png(destination / f"{name}.png", values)
    settings = {"Type": "JsonSerialization", "Version": 1, "ClassName": "TextureSettings",
                "ClassData": {"Preset": "Decal_AlbedoWithOpacity", "SizeReduceLevel": 0,
                              "EnableMipmap": True, "MaintainAlphaCoverage": False}}
    (destination / f"{name}.png.assetinfo").write_text(json.dumps(settings, indent=4) + "\n", encoding="utf-8")
    material = {"materialType": "@gemroot:Atom_Feature_Common@/Assets/Materials/Types/StandardPBR.materialtype",
                "materialTypeVersion": 5, "propertyValues": {
                    "baseColor.textureMap": f"{name}.png",
                    "normal.textureMap": "@gemroot:Atom_Feature_Common@/Assets/Textures/Default/default_normal.tif"}}
    (destination / f"{name}.material").write_text(json.dumps(material, indent=4) + "\n", encoding="utf-8")

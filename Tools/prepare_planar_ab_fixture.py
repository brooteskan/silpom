"""Generate matched planar A/B materials; leaves original TG assets untouched."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import zlib


def read_png_red(path):
    """Decode an un-interlaced 8/16-bit PNG and return normalized red samples."""
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    position, compressed = 8, bytearray()
    while position < len(data):
        length = struct.unpack_from(">I", data, position)[0]
        tag = data[position + 4:position + 8]
        chunk = data[position + 8:position + 8 + length]
        if tag == b"IHDR":
            width, height, bits, kind, compression, filtering, interlace = struct.unpack(">IIBBBBB", chunk)
            assert bits in (8, 16) and kind in (0, 2, 4, 6)
            assert compression == filtering == interlace == 0
        elif tag == b"IDAT":
            compressed.extend(chunk)
        position += length + 12
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[kind]
    bytes_per_sample = bits // 8
    pixel_stride = channels * bytes_per_sample
    row_size = width * pixel_stride
    raw = zlib.decompress(compressed)
    previous = bytearray(row_size)
    result = []
    for y in range(height):
        method = raw[y * (row_size + 1)]
        row = bytearray(raw[y * (row_size + 1) + 1:(y + 1) * (row_size + 1)])
        for x in range(row_size):
            left = row[x - pixel_stride] if x >= pixel_stride else 0
            up = previous[x]
            corner = previous[x - pixel_stride] if x >= pixel_stride else 0
            if method == 0:
                predictor = 0
            elif method == 1:
                predictor = left
            elif method == 2:
                predictor = up
            elif method == 3:
                predictor = (left + up) // 2
            elif method == 4:
                value = left + up - corner
                choices = (left, up, corner)
                predictor = choices[min(range(3), key=lambda i: abs(value - choices[i]))]
            else:
                raise ValueError("Unsupported PNG row filter")
            row[x] = (row[x] + predictor) & 255
        if bits == 8:
            result.extend(row[x * pixel_stride] / 255.0 for x in range(width))
        else:
            result.extend(struct.unpack_from(">H", row, x * pixel_stride)[0] / 65535.0 for x in range(width))
        previous = row
    return width, height, result


def float32_outward(value, upward):
    bits = struct.unpack("<I", struct.pack("<f", value))[0]
    if upward and bits < 0x7f7fffff:
        bits += 1
    elif not upward and bits > 0:
        bits -= 1
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def write_bounds_atlas(source, target):
    """Write conservative periodic bilinear-cell min/max levels to RG32F DDS."""
    width, height, values = read_png_red(source)
    assert width == height and width > 0 and not (width & (width - 1)), "Benchmark bounds require square power-of-two input"
    raw_level = []
    for y in range(height):
        for x in range(width):
            taps = (values[y * width + x], values[y * width + (x + 1) % width],
                    values[((y + 1) % height) * width + x], values[((y + 1) % height) * width + (x + 1) % width])
            raw_level.append((min(taps), max(taps)))
    # PNG and DDS products can expose opposite origins, swapped axes, or a
    # one-cell origin shift depending on the platform image pipeline. Union the
    # complete square symmetry set and its immediate cell neighborhood. Exact
    # height samples still decide every hit; wider bounds only make some rejects
    # less tight and can never introduce an approximate hit.
    level = []
    for y in range(height):
        for x in range(width):
            coordinates = set()
            for cx, cy in ((x, y), (y, x)):
                for sx in (cx, (width - 2 - cx) % width):
                    for sy in (cy, (height - 2 - cy) % height):
                        for oy in (-1, 0, 1):
                            for ox in (-1, 0, 1):
                                coordinates.add(((sx + ox) % width, (sy + oy) % height))
            candidates = [raw_level[cy * width + cx] for cx, cy in coordinates]
            level.append((float32_outward(min(v[0] for v in candidates), False),
                          float32_outward(max(v[1] for v in candidates), True)))
    levels = [(width, height, level)]
    while levels[-1][0] > 1:
        old_width, old_height, old = levels[-1]
        new_width, new_height = old_width // 2, old_height // 2
        reduced = []
        for y in range(new_height):
            for x in range(new_width):
                children = (old[(2*y) * old_width + 2*x], old[(2*y) * old_width + 2*x + 1],
                            old[(2*y+1) * old_width + 2*x], old[(2*y+1) * old_width + 2*x + 1])
                reduced.append((min(v[0] for v in children), max(v[1] for v in children)))
        levels.append((new_width, new_height, reduced))
    packed_height = sum(item[1] for item in levels)
    # A complete power-of-two reduction occupies 2*N-1 rows. Atom's image
    # pipeline rounds that non-power-of-two DDS dimension up and resamples it,
    # shifting the level-zero cell bounds away from their height texels. Keep
    # one unused padding row so the imported image remains exactly 2*N high.
    atlas_height = height * 2
    assert packed_height + 1 == atlas_height
    pixels = bytearray(width * atlas_height * 8)
    y_offset = 0
    for level_width, level_height, samples in levels:
        for y in range(level_height):
            for x in range(level_width):
                # Atom's PNG source pipeline exposes row zero opposite to a
                # directly imported DDS. Store each atlas band bottom-up so a
                # Load(ix,iy) addresses bounds for the same processed texels.
                source_y = level_height - 1 - y
                struct.pack_into("<ff", pixels, ((y_offset + y) * width + x) * 8,
                                 *samples[source_y * level_width + x])
        y_offset += level_height
    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000
    header = struct.pack("<7I", 124, flags, atlas_height, width, width * 8, 0, 0)
    header += bytes(11 * 4)
    header += struct.pack("<II4s5I", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    header += struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    dx10 = struct.pack("<5I", 16, 3, 0, 1, 0)  # R32G32_FLOAT, Texture2D
    target.write_bytes(b"DDS " + header + dx10 + pixels)
    return {"format": "R32G32_FLOAT", "width": width, "height": atlas_height,
            "levels": len(levels), "bytes": len(pixels)}


def prepare(project):
    source = project / "Assets/Materials/StoneWall"
    reference_source = source / "M_StoneWall_Trim_Displacement.material"
    reference = json.loads(reference_source.read_text(encoding="utf-8"))["propertyValues"]
    target = project / "Assets/SilPOMPlanarAB"
    target.mkdir(parents=True, exist_ok=True)
    height = source / reference["vertexDisplacement.textureMap"]
    copied = target / "height.png"
    shutil.copyfile(height, copied)
    bounds = write_bounds_atlas(copied, target / "height_bounds.dds")
    (target / "height_bounds.dds.assetinfo").write_text('''<ObjectStream version="3">
  <Class name="TextureSettings" version="2" type="{980132FF-C450-425D-8AE0-BD96A8486177}">
    <Class name="Name" field="Preset" value="LUT_RG32F" type="{3D2B920C-9EFD-40D5-AAE0-DF131C3D4931}"/>
    <Class name="unsigned int" field="SizeReduceLevel" value="0" type="{43DA906B-7DEF-4CA8-9790-854106D3F983}"/>
    <Class name="bool" field="EngineReduce" value="false" type="{A0CA880C-AFE4-43CB-926C-59AC48496112}"/>
    <Class name="bool" field="EnableMipmap" value="false" type="{A0CA880C-AFE4-43CB-926C-59AC48496112}"/>
  </Class>
</ObjectStream>
''', encoding="utf-8")
    width, height_pixels = struct.unpack(">II", copied.read_bytes()[16:24])
    (target / "height.png.assetinfo").write_text('''<ObjectStream version="3">
  <Class name="TextureSettings" version="2" type="{980132FF-C450-425D-8AE0-BD96A8486177}">
    <Class name="Name" field="Preset" value="LUT_R32F" type="{3D2B920C-9EFD-40D5-AAE0-DF131C3D4931}"/>
    <Class name="unsigned int" field="SizeReduceLevel" value="0" type="{43DA906B-7DEF-4CA8-9790-854106D3F983}"/>
    <Class name="bool" field="EngineReduce" value="false" type="{A0CA880C-AFE4-43CB-926C-59AC48496112}"/>
    <Class name="bool" field="EnableMipmap" value="false" type="{A0CA880C-AFE4-43CB-926C-59AC48496112}"/>
  </Class>
</ObjectStream>
''', encoding="utf-8")
    common = {key: value for key, value in reference.items()
              if not key.startswith(("vertexDisplacement.", "parallax."))}
    for key in ("baseColor.textureMap", "normal.textureMap", "occlusion.diffuseTextureMap"):
        if key in common:
            common[key] = "../Materials/StoneWall/" + common[key]
    common.update({"general.doubleSided": True, "parallax.useTexture": False, "parallax.pdo": False})
    scale = reference.get("vertexDisplacement.scale", .1)
    midpoint = reference.get("vertexDisplacement.midpoint", .5)
    def material(name, kind, props):
        document = {"materialType": kind, "materialTypeVersion": 1, "propertyValues": props}
        (target / name).write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    quad = {**common, "surface.heightMap": "height.png", "surface.heightBounds": "height_bounds.dds", "surface.scale": scale,
            "surface.reference": midpoint, "surface.addressMode": 0, "surface.maxCells": 4096}
    material("QuadBaseline.material", "@gemroot:SilPOM@/Assets/Materials/Types/SilPOM/SilPOM.materialtype",
             {**quad, "surface.reuseTexels": False, "surface.useHierarchy": False})
    material("QuadOptimized.material", "@gemroot:SilPOM@/Assets/Materials/Types/SilPOM/SilPOM.materialtype",
             {**quad, "surface.reuseTexels": True, "surface.useHierarchy": True})
    material("Reference.material", "../Materials/Types/DisplacementPBR/DisplacementPBR.materialtype",
             {**common, "general.castShadows": False, "vertexDisplacement.textureMap": "height.png",
              "vertexDisplacement.textureMapUv": "Unwrapped", "vertexDisplacement.useTexture": True,
              "vertexDisplacement.scale": scale, "vertexDisplacement.midpoint": midpoint,
              "vertexDisplacement.lod": 0.0})
    manifest = {
        "schema": 1, "width_metres": 2.0, "height_metres": 2.0, "scale_metres": scale, "midpoint": midpoint,
        "height_size": [width, height_pixels], "height_sha256": hashlib.sha256(copied.read_bytes()).hexdigest(),
        "height_encoding": "linear single-mip R32_FLOAT; bilinear texel centers; repeat",
        "uv": {"tile": [1, 1], "offset": [0, 0], "rotation": 0}, "max_cells": 4096,
        "reference_source": str(reference_source), "reference_source_sha256": hashlib.sha256(reference_source.read_bytes()).hexdigest(),
        "reference_material": "assets/silpomplanarab/reference.azmaterial",
        "quad_material": "assets/silpomplanarab/quadoptimized.azmaterial",
        "quad_baseline_material": "assets/silpomplanarab/quadbaseline.azmaterial",
        "quad_optimized_material": "assets/silpomplanarab/quadoptimized.azmaterial",
        "variants": ["quad_baseline", "quad_optimized", "reference"],
        "optimization": "precomputed conservative cell bounds for long traversals with exact bilinear leaves",
        "height_bounds": bounds,
        "subdivision_candidates": [32, 64, 128, 256, 512], "resolution": [1920, 1080], "msaa": 2,
        "samples": 600, "runs": 3, "warmup_frames": 120,
        "thresholds": {"agreed": False, "median_improvement": .20, "p95_regression": .05,
                       "silhouette_p95_pixels": 1.0, "depth_p95_metres": .001, "ssim": .98},
        "shadow": "same undisplaced two-triangle SilPOM quad for both variants",
        "common_material": common,
    }
    (target / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2))
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    prepare(parser.parse_args().project.resolve())

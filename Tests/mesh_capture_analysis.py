"""Check that Atom camera depth consumes the staged fixture's GPU hit records.

This is a consumer-consistency check, not an independent surface/root oracle.
The disposable fixture has translation (16,16,40) and a +90 degree X rotation.
"""
import argparse
import json
from pathlib import Path
import struct


def validate_motion_albedo(filename):
    """Detect stale-view diagnostics while still requiring visible fixture color."""
    image = Path(filename).read_bytes()
    assert image[:4] == b"DDS " and image[84:88] == b"DX10"
    height, width = struct.unpack_from("<II", image, 12)
    assert struct.unpack_from("<I", image, 128)[0] == 28, "Expected RGBA8 UNORM albedo"
    assert len(image) == 148 + width * height * 4
    magenta = red = blue = 0
    for r, g, b, a in struct.iter_unpack("4B", image[148:]):
        if a != 255: continue
        # StandardPBR's energy conservation slightly attenuates the diagnostic
        # (245,0,245 on the test adapter), even in the unlit albedo attachment.
        if r >= 230 and g <= 20 and b >= 230 and abs(r - b) <= 10: magenta += 1
        elif r > 1.5 * max(g, b) and r > 32: red += 1
        elif b > 1.5 * max(r, g) and b > 32: blue += 1
    return dict(magenta=magenta, red=red, blue=blue, viewport=[width, height])


def validate_motion_frame(filename, hit_filename, batch_offsets):
    """Reject stale-view magenta; retain and report real bounded exhaustion.

    Both attachments must be requested before yielding the same render frame.
    This is a synchronization test, not a claim that every moving ray certifies.
    """
    check = validate_motion_albedo(filename)
    width, height = check["viewport"]
    image = Path(filename).read_bytes()
    magenta_pixels = {i for i, (r, g, b, a) in enumerate(struct.iter_unpack("4B", image[148:]))
                      if a == 255 and r >= 230 and g <= 20 and b >= 230 and abs(r - b) <= 10}
    hits = Path(hit_filename).read_bytes()
    exhausted_pixels = set()
    states = [0, 0, 0, 0]
    view_errors = 0
    camera_views = 0
    for base in batch_offsets:
        count = struct.unpack_from("<I", hits, base)[0]
        assert 0 < count <= 32
        for view in range(count):
            header = base + 16 + view * 128
            w, h, near_z, error = struct.unpack_from("<4f", hits, header + 64)
            if near_z != 1: continue
            camera_views += 1
            assert (w, h) == (width, height)
            view_errors += int(error != 0)
            if error: continue
            left, top, columns, rows = struct.unpack_from("<4I", hits, header + 80)
            assert left + columns <= width and top + rows <= height
            offset = struct.unpack_from("<I", hits, header + 96)[0]
            for sample in range(columns * rows):
                status = struct.unpack_from("<I", hits, offset + sample * 96 + 12)[0]
                assert 0 <= status <= 3
                states[status] += 1
                if status == 2:
                    exhausted_pixels.add((top + sample // columns) * width + left + sample % columns)
    check.update(camera_views=camera_views, certified_hits=states[1], exhausted=states[2],
                 invalid=states[3], view_errors=view_errors,
                 unexplained_magenta=len(magenta_pixels - exhausted_pixels))
    return check


def motion_frame_passes(check):
    return (check["camera_views"] > 0 and check["certified_hits"] > 0
            and check["red"] > 0 and check["blue"] > 0
            and check["view_errors"] == 0 and check["invalid"] == 0
            and check["unexplained_magenta"] == 0)


def validate_depth(folder, name, reports, shadow=False):
    folder = Path(folder)
    image = (folder / (name + ("_shadow.dds" if shadow else "_depth.dds"))).read_bytes()
    assert image[:4] == b"DDS " and image[84:88] == b"DX10"
    height, width = struct.unpack_from("<II", image, 12)
    assert struct.unpack_from("<I", image, 128)[0] in (40, 41), "Expected D32/R32 float depth"
    assert len(image) == 148 + width * height * 4
    hits = (folder / (name + "_hits.bin")).read_bytes()
    expected = {}
    projection_error = 0.0
    for report in reports:
        if report["shadow"] != shadow: continue
        header = report["batch_offset"] + 16 + report["view"] * 128
        matrix = struct.unpack_from("<16f", hits, header)
        left, top, columns, rows = struct.unpack_from("<4I", hits, header + 80)
        records = struct.unpack_from("<I", hits, header + 96)[0]
        for sample in range(columns * rows):
            address = records + sample * 96
            if struct.unpack_from("<I", hits, address + 12)[0] != 1: continue
            x, y, z = struct.unpack_from("<3f", hits, address)
            world = (16+x, 16-z, 40+y, 1)
            clip_z = sum(matrix[8+i] * world[i] for i in range(4))
            clip_w = sum(matrix[12+i] * world[i] for i in range(4))
            depth = struct.unpack_from("<f", hits, address + 64)[0]
            projection_error = max(projection_error, abs(depth - clip_z / clip_w))
            pixel = (top + sample // columns) * width + left + sample % columns
            nearest = min if shadow else max
            expected[pixel] = nearest(expected.get(pixel, 1 if shadow else 0), depth)
    matching = closer = farther = 0
    minimum = maximum = 0.0
    for pixel, depth in expected.items():
        actual = struct.unpack_from("<f", image, 148 + pixel * 4)[0]
        delta = (depth - actual) if shadow else (actual - depth)  # positive is nearer
        minimum = min(minimum, delta); maximum = max(maximum, delta)
        if delta == 0: matching += 1
        elif delta > 0: closer += 1  # another ordinary/curved face may occlude it
        else: farther += 1
    return dict(samples=len(expected), matching=matching, nearer_occluders=closer,
                missing_or_farther=farther, min_delta=minimum, max_delta=maximum,
                max_projection_error=projection_error)


def validate_albedo(folder, name, reports):
    """Require the fixture's red/blue forward outputs at visible curved hits."""
    folder = Path(folder)
    image = (folder / (name + "_albedo.dds")).read_bytes()
    assert image[:4] == b"DDS " and image[84:88] == b"DX10"
    height, width = struct.unpack_from("<II", image, 12)
    assert struct.unpack_from("<I", image, 128)[0] == 28, "Expected RGBA8 UNORM albedo"
    assert len(image) == 148 + width * height * 4
    depths = (folder / (name + "_depth.dds")).read_bytes()
    hits = (folder / (name + "_hits.bin")).read_bytes()
    expected = {}
    for report in reports:
        if report["shadow"]: continue
        header = report["batch_offset"] + 16 + report["view"] * 128
        left, top, columns, rows = struct.unpack_from("<4I", hits, header + 80)
        records = struct.unpack_from("<I", hits, header + 96)[0]
        for sample in range(columns * rows):
            address = records + sample * 96
            if struct.unpack_from("<I", hits, address + 12)[0] != 1: continue
            depth = struct.unpack_from("<f", hits, address + 64)[0]
            primitive = struct.unpack_from("<I", hits, address + 28)[0]
            pixel = (top + sample // columns) * width + left + sample % columns
            if pixel not in expected or depth > expected[pixel][0]:
                expected[pixel] = (depth, primitive)
    red = blue = occluded = wrong = 0
    for pixel, (depth, primitive) in expected.items():
        if struct.unpack_from("<f", depths, 148 + pixel * 4)[0] != depth:
            occluded += 1
            continue
        r, g, b, a = image[148 + pixel * 4:152 + pixel * 4]
        if primitive < 2 and r > g and r > b and a == 255: red += 1
        elif primitive == 2 and b > r and b > g and a == 255: blue += 1
        else: wrong += 1
    return dict(red=red, blue=blue, occluded=occluded, wrong_or_missing=wrong)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("folder", type=Path)
    args = parser.parse_args()
    report = json.loads((args.folder / "result.json").read_text())
    checks = {name: validate_depth(args.folder, name, rows) for name, rows in report["gpu_hits"].items()}
    print(json.dumps(checks, indent=2))
    assert all(v["matching"] and not v["missing_or_farther"] and v["max_projection_error"] < 1e-5
               for v in checks.values()), "Raster depth disagrees with certified hits"
    colors = {name: validate_albedo(args.folder, name, rows) for name, rows in report["gpu_hits"].items()}
    print(json.dumps(colors, indent=2))
    assert all(v["red"] + v["blue"] and not v["wrong_or_missing"] for v in colors.values()), "Missing forward albedo"
    shadows = {name: validate_depth(args.folder, name, rows, shadow=True) for name, rows in report["gpu_hits"].items()}
    print(json.dumps(shadows, indent=2))
    assert all(v["matching"] and not v["missing_or_farther"] and v["max_projection_error"] < 1e-5
               for v in shadows.values()), "Shadow depth disagrees with certified hits"
    if "motion_checks" in report:
        motion = [validate_motion_frame(row["capture"], row["hit_capture"], row["batch_offsets"])
                  for row in report["motion_checks"]]
        print(json.dumps(motion, indent=2))
        assert len(motion) == 12 and all(motion_frame_passes(v) for v in motion), "Moving camera lost forward output"
        assert all(row["moving_ticks"] > 0 for row in report["motion_checks"]), "Camera did not move"

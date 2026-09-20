"""Summarize raw planar A/B GPU queries and native-resolution image/depth pairs.

Timing analysis uses the standard library; image analysis additionally uses numpy.
Leaf-pass sums are GPU work, NOT elapsed frame time (queues can overlap). Parent
passes are never added to their children. Missing readbacks are reported.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import statistics
import struct
import zlib


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        raise ValueError("No samples")
    index = (len(ordered) - 1) * fraction
    low = int(index)
    return ordered[low] + (ordered[min(low + 1, len(ordered) - 1)] - ordered[low]) * (index - low)


def distribution(values):
    return {"count": len(values), "median_ms": statistics.median(values),
            "p95_ms": percentile(values, .95), "min_ms": min(values), "max_ms": max(values)}


def summarize_capture(document):
    assert document["complete"] and document["frames"] == document["requested"], "Incomplete capture"
    per_pass = defaultdict(list)
    per_frame = defaultdict(float)
    seen = set()
    frame_passes = set()
    for frame, index, begin, ticks, ns in document["samples"]:
        assert 0 <= frame < document["frames"] and 0 <= index < len(document["passes"])
        assert begin > 0 and ticks >= 0 and ns >= 0
        assert (index, begin) not in seen, "Duplicate stale GPU timestamp"
        assert (frame, index) not in frame_passes, "Duplicate pass in readback frame"
        seen.add((index, begin))
        frame_passes.add((frame, index))
        info = document["passes"][index]
        if not info["parent"]:
            per_pass[info["path"]].append(ns / 1e6)
            per_frame[frame] += ns / 1e6
    assert len(per_frame) == document["frames"], "Missing frame"
    summary = {"passes": {name: distribution(values) for name, values in per_pass.items()},
            "leaf_work_sum_not_frame_time": distribution(list(per_frame.values())),
            "partial_passes": [name for name, values in per_pass.items() if len(values) != document["frames"]],
            "unmeasured_passes": [p["path"] for p in document["passes"]
                                  if not p["parent"] and p["path"] not in per_pass]}
    if document.get("graphics_span_ns"):
        assert len(document["graphics_span_ns"]) == document["frames"]
        summary["graphics_pass_span"] = distribution([ns / 1e6 for ns in document["graphics_span_ns"]])
    return summary


def read_depth(path):
    import numpy as np
    data = path.read_bytes()
    assert data[:4] == b"DDS " and struct.unpack_from("<I", data, 4)[0] == 124
    height, width = struct.unpack_from("<II", data, 12)
    fourcc = data[84:88]
    if fourcc == b"DX10":
        format_id = struct.unpack_from("<I", data, 128)[0]
        assert format_id == 41, f"Expected DXGI R32_FLOAT, got {format_id}"
        offset = 148
    else:
        assert struct.unpack("<I", fourcc)[0] == 114, "Expected D3DFMT_R32F"
        offset = 128
    return np.frombuffer(data, dtype="<f4", count=width * height, offset=offset).reshape(height, width)


def read_png(path):
    """Read unmodified 8-bit RGB(A) PNG captures, including all PNG row filters."""
    import numpy as np
    # Optional native decoder speeds up large sweeps; the fallback keeps NumPy
    # as the only required image-analysis dependency.
    try:
        from PIL import Image
    except ImportError:
        pass
    else:
        with Image.open(path) as image:
            assert image.mode in ("RGB", "RGBA")
            return np.asarray(image.convert("RGB"), dtype=np.float64) / 255
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    position, compressed = 8, bytearray()
    while position < len(data):
        length = struct.unpack_from(">I", data, position)[0]
        tag, chunk = data[position + 4:position + 8], data[position + 8:position + 8 + length]
        if tag == b"IHDR":
            width, height, bits, kind, compression, filtering, interlace = struct.unpack(">IIBBBBB", chunk)
            assert bits == 8 and kind in (2, 6) and interlace == 0 and compression == 0 and filtering == 0
        elif tag == b"IDAT":
            compressed.extend(chunk)
        position += length + 12
    channels = 3 if kind == 2 else 4
    stride = width * channels
    raw = zlib.decompress(compressed)
    assert len(raw) == height * (stride + 1)
    rows = np.empty((height, stride), dtype=np.uint8)
    previous = bytearray(stride)
    for y in range(height):
        method = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if method == 2:
            rows[y] = np.frombuffer(row, dtype=np.uint8) + np.frombuffer(previous, dtype=np.uint8)
            previous = bytearray(rows[y])
            continue
        for x in range(stride) if method else ():
            left = row[x - channels] if x >= channels else 0
            up = previous[x]
            corner = previous[x - channels] if x >= channels else 0
            if method == 1:
                predictor = left
            elif method == 3:
                predictor = (left + up) // 2
            elif method == 4:
                value = left + up - corner
                distances = (abs(value - left), abs(value - up), abs(value - corner))
                predictor = (left, up, corner)[distances.index(min(distances))]
            else:
                raise ValueError("Unknown PNG filter")
            row[x] = (row[x] + predictor) & 255
        rows[y] = np.frombuffer(row, dtype=np.uint8)
        previous = row
    return rows.reshape(height, width, channels)[..., :3].astype(np.float64) / 255


def box_mean(image, radius=5):
    import numpy as np
    size = 2 * radius + 1
    padded = np.pad(image, radius, mode="reflect")
    integral = np.pad(padded, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    return (integral[size:, size:] - integral[:-size, size:] -
            integral[size:, :-size] + integral[:-size, :-size]) / (size * size)


def silhouette_distance(first, second):
    """Exact symmetric Euclidean distance between foreground boundary pixels."""
    import numpy as np
    def boundary(mask):
        padded = np.pad(mask, 1)
        interior = (padded[:-2, 1:-1] & padded[2:, 1:-1] &
                    padded[1:-1, :-2] & padded[1:-1, 2:])
        return np.column_stack(np.nonzero(mask & ~interior)).astype(np.float64)
    a, b = boundary(first), boundary(second)
    assert len(a) and len(b), "Empty silhouette"
    distances = []
    for source, target in ((a, b), (b, a)):
        for start in range(0, len(source), 256):
            delta = source[start:start + 256, None, :] - target[None, :, :]
            distances.extend(np.sqrt(np.einsum("ijk,ijk->ij", delta, delta).min(axis=1)))
    return {"silhouette_p95_pixels": float(np.quantile(distances, .95)),
            "silhouette_max_pixels": float(max(distances))}


def image_metrics(first, second, depth_first, depth_second):
    import numpy as np
    assert first.shape == second.shape and first.shape[:2] == depth_first.shape == depth_second.shape
    # Fixture is an isolated wall 3m from the eye; level geometry is >5m away.
    # Not a general-purpose segmentation method. Keep this fixture assumption explicit.
    a = np.isfinite(depth_first) & (depth_first > 0) & (depth_first < 5)
    b = np.isfinite(depth_second) & (depth_second > 0) & (depth_second < 5)
    union, both = a | b, a & b
    assert both.sum() > 100, "No comparable relief pixels; check linear depth encoding"
    luma_a, luma_b = first @ np.array([.2126, .7152, .0722]), second @ np.array([.2126, .7152, .0722])
    ma, mb = box_mean(luma_a), box_mean(luma_b)
    va = np.maximum(0, box_mean(luma_a * luma_a) - ma * ma)
    vb = np.maximum(0, box_mean(luma_b * luma_b) - mb * mb)
    cov = box_mean(luma_a * luma_b) - ma * mb
    ssim = ((2 * ma * mb + .01 ** 2) * (2 * cov + .03 ** 2) /
            ((ma * ma + mb * mb + .01 ** 2) * (va + vb + .03 ** 2)))
    difference = np.abs(depth_first[both] - depth_second[both])
    return {**silhouette_distance(a, b),
            "roi_pixels": int(union.sum()), "silhouette_iou": float(both.sum() / union.sum()),
            "silhouette_disagreement_pixels": int((a ^ b).sum()),
            "depth_median_metres": float(np.median(difference)),
            "depth_p95_metres": float(np.quantile(difference, .95)),
            "depth_max_metres": float(difference.max()),
            "ssim_luma_roi_box11": float(ssim[union].mean()),
            "rgb_mae_roi": float(np.abs(first[union] - second[union]).mean()),
            "segmentation": "0 < linear depth < 5m; isolated 3m wall fixture"}


def diagnostic_pixels(image, roi=None):
    import numpy as np
    if roi is None:
        roi = np.ones(image.shape[:2], dtype=bool)
    return {"exhausted_pixels": int(np.count_nonzero(
                roi & (image[..., 0] > .75) & (image[..., 1] < .2) & (image[..., 2] > .75))),
            "invalid_pixels": int(np.count_nonzero(
                roi & (image[..., 0] > .75) & (image[..., 1] > .75) & (image[..., 2] < .2)))}


def analyze(folder, images=False):
    result = json.loads((folder / "result.json").read_text())
    report = {"harness_passed": result["passed"], "accepted": False,
              "acceptance_note": "Requires agreed workload, sufficient reference density, quality review and elapsed GPU timing.",
              "captures": {}, "image_comparisons": {}, "aggregates": {},
              "diagnostics": result.get("diagnostics", {}), "shadow_comparisons": {}}
    for cells in result["grids"]:
        files = [folder / f"grid{cells}_front_{variant}_run0_shadow.dds"
                 for variant in ("quad", "reference")]
        if all(path.exists() for path in files):
            hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in files]
            report["shadow_comparisons"][f"grid{cells}_front"] = {
                "identical": hashes[0] == hashes[1], "sha256": dict(zip(("quad", "reference"), hashes)),
                "scope": "Captured shadow attachment slice only; not an all-cascade visibility oracle"}
    groups = defaultdict(lambda: defaultdict(list))
    for name in result["timings"]:
        raw = json.loads((folder / name).read_text())
        report["captures"][name] = {**{k: raw[k] for k in ("variant", "view", "run", "cells", "triangles")},
                                    **summarize_capture(raw)}
        group = groups[f"grid{raw['cells']}_{raw['view']}_{raw['variant']}"]
        group["graphics_pass_span"].extend(ns / 1e6 for ns in raw.get("graphics_span_ns", []))
        for _, index, _, _, ns in raw["samples"]:
            info = raw["passes"][index]
            if not info["parent"]:
                group[info["path"]].append(ns / 1e6)
    report["aggregates"] = {key: {name: distribution(values) for name, values in metrics.items() if values}
                            for key, metrics in groups.items()}
    if images:
        for cells in result["grids"]:
            for view in result["views"]:
                prefix = f"grid{cells}_{view}"
                report["image_comparisons"][prefix] = image_metrics(
                    read_png(folder / (prefix + "_quad_run0.png")),
                    read_png(folder / (prefix + "_reference_run0.png")),
                    read_depth(folder / (prefix + "_quad_run0_depth.dds")),
                    read_depth(folder / (prefix + "_reference_run0_depth.dds")))
                import numpy as np
                mask = read_png(folder / (prefix + "_hit_mask.png"))
                depth = read_depth(folder / (prefix + "_quad_run0_depth.dds"))
                roi = np.isfinite(depth) & (depth > 0) & (depth < 5)
                report["image_comparisons"][prefix].update(diagnostic_pixels(mask, roi))
        forced = folder / "forced_exhaustion.png"
        if forced.exists():
            # This whole-frame check only confirms the visible magenta signal.
            # Unrelated yellow scene/overlay pixels are not invalid shader hits.
            report["diagnostics"]["forced_exhaustion_magenta_pixels"] = diagnostic_pixels(
                read_png(forced))["exhausted_pixels"]
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--images", action="store_true")
    args = parser.parse_args()
    report = analyze(args.folder, args.images)
    (args.folder / "analysis.json").write_text(json.dumps(report, indent=2) + "\n")
    for name, aggregate in report["aggregates"].items():
        print(name)
        for path in ("graphics_pass_span", "Root.MainPipeline_0.DepthPrePass.DepthPass", "Root.MainPipeline_0.OpaquePass.Forward"):
            if path in aggregate:
                print("  ", path, json.dumps(aggregate[path]))
    print(json.dumps(report["image_comparisons"], indent=2))

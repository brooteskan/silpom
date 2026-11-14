"""Prepare exact stone-wall labels and bake an experimental learned hit cache."""
import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import time

os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
os.environ.setdefault("OMP_NUM_THREADS", "1")

import numpy as np
from PIL import Image

GRAZING_X = (2.8, 12.0)
GRAZING_Y = (-2.8, 2.8)


def features(queries):
    q = np.asarray(queries, np.float32)
    values = [q[:, :2] * 2 - 1, q[:, 2:3] / 9 * 2 - 1, q[:, 3:4] / 2]
    for frequency in (1, 2, 4, 8, 16, 32, 64):
        phase = np.float32(math.pi * frequency) * q[:, :2] * 2
        values.extend((np.sin(phase), np.cos(phase)))
    return np.column_stack(values).astype(np.float32)


def trace(dll, height, rays):
    call = ctypes.CDLL(str(dll)).TraceStoneWall
    pointer = np.ctypeslib.ndpointer(dtype=np.float32, flags="C_CONTIGUOUS")
    call.argtypes = [pointer, ctypes.c_uint, ctypes.c_uint, pointer, ctypes.c_uint, pointer]
    call.restype = None
    rays = np.ascontiguousarray(rays, np.float32)
    output = np.zeros((len(rays), 8), np.float32)
    call(height, height.shape[1], height.shape[0], rays, len(rays), output)
    return output


def prepare(project, dll, destination, count=32768):
    source = project / "Assets/SilPOMPlanarAB/height.png"
    image = np.asarray(Image.open(source))
    if image.ndim == 3:
        image = image[:, :, 0]
    height = np.ascontiguousarray(image, np.float32)
    height /= 65535 if image.dtype.itemsize > 1 else 255
    rng = np.random.default_rng(20261107)
    queries = np.column_stack((
        rng.uniform(0, 1, count), rng.uniform(0, 1, count),
        rng.uniform(.35, 9, count), rng.uniform(-1.6, 1.6, count),
    )).astype(np.float32)
    # Require both ends of the relief interval inside the finite patch.
    bottom = queries[:, :2] - queries[:, 2:4] * .05
    inside = np.all((bottom > 0) & (bottom < 1), axis=1)
    queries = queries[inside]
    rays = np.column_stack((queries[:, :2] * 2 - 1,
                            np.full(len(queries), .09500093),
                            -queries[:, 2:4], -np.ones(len(queries)))).astype(np.float32)
    rays[:, 3:] /= np.linalg.norm(rays[:, 3:], axis=1)[:, None]
    hits = trace(dll, height, rays)
    if not np.all(hits[:, 0] == 1):
        raise RuntimeError("The interior teacher rays must all hit the finite surface")
    target = (hits[:, 4:5] / .1 + .05000000074505806).astype(np.float32)
    destination.mkdir(parents=True, exist_ok=True)
    train_end = int(len(queries) * .8)
    validation_end = int(len(queries) * .9)
    np.savez(destination / "dataset.npz", queries=queries, features=features(queries),
             target=target, rays=rays, exact=hits,
             split=np.r_[np.zeros(train_end), np.ones(validation_end-train_end),
                         np.full(len(queries)-validation_end, 2)].astype(np.int32))
    manifest = {
        "heightSha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "teacherShaderSha256": hashlib.sha256(
            (project / "Gems/silpom/Assets/Shaders/SilPOM/Heightfield.azsli").read_bytes()).hexdigest(),
        "seed": 20261107, "rows": len(queries), "features": features(queries).shape[1],
        "target": "normalized height of the exact nearest intersection",
        "domain": {"u": [0, 1], "v": [0, 1], "slopeX": [.35, 9], "slopeY": [-1.6, 1.6]},
        "fallback": "original exact solver unless both vertical-interval endpoints are inside the patch",
    }
    (destination / "dataset.json").write_text(json.dumps(manifest, indent=2) + "\n")
    # Broo's advertised small-workload contract has a 1,024-row limit.
    small_ids = np.r_[np.arange(820), np.arange(train_end, train_end + 204)]
    small = {
        "x": [[round(float(v), 5) for v in row[:16]] for row in features(queries[small_ids])],
        "y": [[round(float(v), 6) for v in row] for row in target[small_ids]],
        "split": {"train": list(range(820)), "evaluation": list(range(820, 1024))},
        "preprocessing": {"kind": "identity"},
    }
    (destination / "broo-dataset.json").write_text(json.dumps(small, separators=(",", ":")))
    print(json.dumps(manifest))


def prepare_angular_dataset(project, dll, destination, slope_x, slope_y, focus_grazing=False):
    source = project / "Assets/SilPOMPlanarAB/height.png"
    image = np.asarray(Image.open(source))
    if image.ndim == 3:
        image = image[:, :, 0]
    height = np.ascontiguousarray(image, np.float32) / (65535 if image.dtype.itemsize > 1 else 255)
    rng = np.random.default_rng(20261108)
    count = 262144
    slopes = np.column_stack((rng.uniform(*slope_x, count), rng.uniform(*slope_y, count)))
    if focus_grazing:
        focused_count = count * 3
        angles = np.clip(rng.normal(80, 2.2, focused_count), 74, 85)
        sign = -1 if slope_x[1] < 0 else 1
        horizontal = sign * np.tan(np.deg2rad(angles))
        vertical = np.clip(rng.normal(0, 1.05, focused_count), *slope_y)
        slopes = np.concatenate((slopes, np.column_stack((horizontal, vertical))))
        count += focused_count
    offset = slopes * .05
    entry = rng.uniform(0, 1, (count, 2))
    uv = entry * (1 + abs(offset)) + np.minimum(offset, 0)
    rays = np.column_stack((uv * 2 - 1, np.full(count, .095001), -slopes, -np.ones(count)))
    rays[:, 3:] /= np.linalg.norm(rays[:, 3:], axis=1)[:, None]
    hits = trace(dll, height, rays)
    hit = hits[:, 0] == 1
    # Reuse the existing network and atlas layout with normalized grazing inputs.
    u = (uv - np.minimum(offset, 0)) / (1 + abs(offset))
    x = np.interp(slopes[:, 0], slope_x, (.35, 9))
    y = np.interp(slopes[:, 1], slope_y, (-1.6, 1.6))
    queries = np.column_stack((u, x, y))[hit].astype(np.float32)
    target = (hits[hit, 4:5] / .1 + .05).astype(np.float32)
    split = np.searchsorted([.8, .9], rng.uniform(0, 1, len(target)))
    destination.mkdir(parents=True, exist_ok=True)
    np.savez(destination / "dataset.npz", queries=queries, target=target, split=split)
    manifest = {"heightSha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "samples": count, "hits": len(target), "slopeX": slope_x, "slopeY": slope_y}
    if focus_grazing:
        manifest["focusedSamples"] = focused_count
        manifest["focusedHorizontalAngleDegrees"] = {"mean": sign * 80, "standardDeviation": 2.2,
                                                       "absoluteRange": [74, 85]}
    (destination / "dataset.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest), flush=True)


def prepare_grazing(project, dll, destination):
    prepare_angular_dataset(project, dll, destination, GRAZING_X, GRAZING_Y)


def predict(model, queries):
    x = features(queries)
    first = np.asarray(model["first"], np.float32)
    second = np.asarray(model["second"], np.float32)
    h = np.tanh(x @ first[:, :-1].T + first[:, -1])
    return 1 / (1 + np.exp(-np.clip(h @ second[:-1] + second[-1], -30, 30)))


def train(dataset, destination, initial_model=None, steps=30000):
    data = np.load(dataset / "dataset.npz")
    x = features(data["queries"])
    y = data["target"].ravel()
    train_ids = np.flatnonzero(data["split"] == 0)
    validation_ids = np.flatnonzero(data["split"] == 1)
    test_ids = np.flatnonzero(data["split"] == 2)
    rng = np.random.default_rng(20261107)
    hidden = 256
    rate_scale = 1.0
    if initial_model is None:
        first = rng.normal(0, .12, (hidden, x.shape[1] + 1)).astype(np.float32)
        first[:, -1] = 0
        second = rng.normal(0, .03, hidden + 1).astype(np.float32)
    else:
        previous = json.loads(initial_model.read_text())
        first = np.asarray(previous["first"], dtype=np.float32)
        second = np.asarray(previous["second"], dtype=np.float32)
        hidden = len(first)
        rate_scale = .5
    weights = np.r_[first.ravel(), second]
    momentum = np.zeros_like(weights)
    variance = np.zeros_like(weights)
    start = time.monotonic()
    history = []
    for step in range(1, steps + 1):
        ids = rng.choice(train_ids, 512)
        a, target = x[ids], y[ids]
        first = weights[:hidden * (x.shape[1] + 1)].reshape(hidden, -1)
        second = weights[hidden * (x.shape[1] + 1):]
        h = np.tanh(a @ first[:, :-1].T + first[:, -1])
        output = 1 / (1 + np.exp(-np.clip(h @ second[:-1] + second[-1], -30, 30)))
        dz = 2 / len(ids) * (output - target) * output * (1 - output)
        dh = dz[:, None] * second[None, :-1] * (1 - h * h)
        gradient = np.r_[np.column_stack((dh.T @ a, dh.sum(axis=0))).ravel(),
                         h.T @ dz, dz.sum()]
        momentum = .9 * momentum + .1 * gradient
        variance = .999 * variance + .001 * gradient * gradient
        rate = rate_scale * (.002 if step <= steps * 2/3 else (.0007 if step <= steps * .9 else .0002))
        weights -= rate * (momentum / (1 - .9 ** step)) / (
            np.sqrt(variance / (1 - .999 ** step)) + 1e-8)
        if step % 3000 == 0:
            first = weights[:hidden * (x.shape[1] + 1)].reshape(hidden, -1)
            second = weights[hidden * (x.shape[1] + 1):]
            model = {"first": first.tolist(), "second": second.tolist()}
            error = predict(model, data["queries"][validation_ids]) - y[validation_ids]
            row = {"step": step, "seconds": time.monotonic() - start,
                   "validationHeightRmseMetres": float(np.sqrt(np.mean(error ** 2)) * .1)}
            history.append(row)
            print(json.dumps(row), flush=True)
    first = weights[:hidden * (x.shape[1] + 1)].reshape(hidden, -1)
    second = weights[hidden * (x.shape[1] + 1):]
    model = {"schema": 1, "features": x.shape[1], "hidden": hidden,
             "architecture": "Fourier features -> 256 tanh -> sigmoid height",
             "first": first.tolist(), "second": second.tolist(),
             "seed": 20261107, "steps": steps, "trainingDevice": "local CPU",
             "checkpointSelection": "fixed final step; test split excluded from training"}
    if initial_model is not None:
        model["initialModelSha256"] = hashlib.sha256(initial_model.read_bytes()).hexdigest()
    error = predict(model, data["queries"][test_ids]) - y[test_ids]
    metrics = {"testRays": len(test_ids), "testHeightRmseMetres": float(np.sqrt(np.mean(error ** 2)) * .1),
               "testHeightP95Metres": float(np.quantile(abs(error), .95) * .1),
               "testHeightMaxMetres": float(np.max(abs(error)) * .1),
               "seconds": time.monotonic() - start}
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "model.json").write_text(json.dumps(model, separators=(",", ":")) + "\n")
    (destination / "training.json").write_text(json.dumps(
        {"dataset": json.loads((dataset / "dataset.json").read_text()),
         "metrics": metrics, "history": history}, indent=2) + "\n")
    print(json.dumps(metrics), flush=True)


def evaluate_atlas(model_file, x_slopes=16):
    model = json.loads(model_file.read_text())
    resolution, y_slopes = 128, 8
    atlas = np.empty((resolution * y_slopes, resolution * x_slopes), np.float32)
    u, v = np.meshgrid(np.linspace(0, 1, resolution), np.linspace(0, 1, resolution))
    for yi, sy in enumerate(np.linspace(-1.6, 1.6, y_slopes)):
        for xi, sx in enumerate(np.linspace(.35, 9, x_slopes)):
            q = np.column_stack((u.ravel(), v.ravel(), np.full(u.size, sx), np.full(u.size, sy)))
            tile = predict(model, q).reshape(resolution, resolution)
            atlas[yi * resolution:(yi + 1) * resolution,
                  xi * resolution:(xi + 1) * resolution] = tile
    return atlas


def atlas_dds(atlas):
    height, width = atlas.shape
    header = struct.pack("<7I11I8I5I", 124, 0x100F, height, width, width * 4, 0, 1,
                         *([0] * 11), 32, 4, 0x30315844, 0, 0, 0, 0, 0,
                         0x1000, 0, 0, 0, 0)
    return b"DDS " + header + struct.pack("<5I", 41, 3, 0, 1, 0) + atlas.astype("<f4").tobytes()


def bake(destination, grazing=False):
    model_file = destination / "model.json"
    atlas = evaluate_atlas(model_file)
    dds = atlas_dds(atlas)
    (destination / "HitCache.dds").write_bytes(dds)
    (destination / "cache.json").write_text(json.dumps({
        "modelSha256": hashlib.sha256(model_file.read_bytes()).hexdigest(),
        "cacheSha256": hashlib.sha256(dds).hexdigest(), "uvResolution": 128,
        "slopeGrid": [16, 8], "bytes": len(dds),
        "format": "single-mip linear R32_FLOAT atlas", "domainSlopeX": GRAZING_X if grazing else [.35, 9],
        "domainSlopeY": GRAZING_Y if grazing else [-1.6, 1.6]}, indent=2) + "\n")
    print("Baked", atlas.shape, len(dds), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--teacher", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--dataset", type=Path)
    parser.add_argument("--mode", choices=("prepare", "train", "bake"), default="prepare")
    parser.add_argument("--grazing", action="store_true", help="Prepare or bake the side-entry grazing cache")
    args = parser.parse_args()
    if args.mode == "prepare":
        prepare_dataset = prepare_grazing if args.grazing else prepare
        prepare_dataset(args.project, args.teacher, args.output)
    elif args.mode == "train":
        train(args.dataset, args.output)
        bake(args.output, args.grazing)
    else:
        bake(args.output, args.grazing)

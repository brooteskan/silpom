"""Train independent bounded hit-seed models from the welded tower manifest."""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path

os.environ.setdefault("OPENBLAS_NUM_THREADS", "2")
import numpy as np
from PIL import Image


def dot(a, b):
    return np.sum(a * b, axis=-1)


def normalized(a):
    return a / np.linalg.norm(a, axis=-1, keepdims=True)


class Profile:
    def __init__(self, config, height_file, xyz, uv, directions):
        assert config["tiling"] == [1, 1] and config["offset"] == [0, 0]
        self.height = np.ascontiguousarray(np.asarray(Image.open(height_file), dtype=np.float32) / 65535)
        self.atlas = np.array(self.height.shape[::-1])
        self.scale = config["scale_metres"]
        self.reference = config["reference"]
        self.address = {"repeat": 0, "clamp": 1}[config["address"]]
        self.h0 = -self.scale * self.reference
        self.h1 = self.scale * (1 - self.reference)
        self.xyz = xyz
        self.uv = uv
        self.directions = directions
        self.base = xyz - xyz[:, :1]
        self.du, self.dv = np.linalg.solve(uv[:, 1:] - uv[:, :1], self.base[:, 1:]).transpose(1, 0, 2)
        self.inv_uv = np.linalg.inv(np.transpose(uv[:, 1:] - uv[:, :1], (0, 2, 1)))
        self.normal = normalized(np.cross(self.base[:, 1], self.base[:, 2]))
        self.right = normalized(self.du)
        self.up = np.cross(self.normal, self.right)
        self.constant = np.sum((directions[:, 1:] - directions[:, :1])**2, axis=(1, 2)) < 1e-12
        self.extrema = np.concatenate((xyz + directions * self.h0, xyz + directions * self.h1), axis=1)
        self.packed = np.ascontiguousarray(np.column_stack((
            xyz[:, 0], self.du, self.dv, uv.reshape(-1, 6), directions.reshape(-1, 9), self.normal
        )), dtype=np.float32)

    def project(self, ids, direction, relative, ray):
        u, v = self.du[ids], self.dv[ids]
        z = np.cross(u, v)
        axes = np.stack((np.cross(v, direction), np.cross(direction, u), z), axis=1)
        axes /= dot(direction, z)[:, None, None]
        return np.einsum("nij,nj->ni", axes, relative), np.einsum("nij,nj->ni", axes, ray)

    def weights(self, ids, uv_offset):
        bc = np.einsum("nij,n...j->n...i", self.inv_uv[ids], uv_offset)
        return np.concatenate((1 - bc.sum(axis=-1, keepdims=True), bc), axis=-1)

    def sample(self, coords):
        q = coords * self.atlas - .5
        if self.address:
            q = np.clip(q, 0, self.atlas - 1)
        integer = np.floor(q).astype(np.int32)
        f, g = np.moveaxis(q - integer, -1, 0)
        if self.address:
            x, y = np.moveaxis(integer, -1, 0)
            xx, yy = np.moveaxis(np.minimum(integer + 1, self.atlas - 1), -1, 0)
        else:
            x, y = np.moveaxis(integer % self.atlas, -1, 0)
            xx, yy = np.moveaxis((integer + 1) % self.atlas, -1, 0)
        return ((self.height[y, x] * (1 - f) + self.height[y, xx] * f) * (1 - g)
                + (self.height[yy, x] * (1 - f) + self.height[yy, xx] * f) * g)

    def dataset(self, trace, count, seed, retain_rays=False):
        rng = np.random.default_rng(seed)
        ids = rng.integers(len(self.xyz), size=count, dtype=np.uint32)
        b = rng.random((count, 2))
        b[b.sum(axis=1) > 1] = 1 - b[b.sum(axis=1) > 1]
        bary = np.column_stack((1 - b.sum(axis=1), b))
        n = count // 2
        edge = rng.integers(0, 3, size=n)
        w = rng.uniform(-.015, .06, size=n)
        q = rng.random(n) * (1 - w)
        row = np.arange(n)
        bary[row, edge], bary[row, (edge + 1) % 3], bary[row, (edge + 2) % 3] = w, q, 1 - w - q
        directions = self.directions[ids]
        point = np.einsum("ni,nij->nj", bary, self.xyz[ids])
        point += np.einsum("ni,nij->nj", bary, directions) * rng.uniform(self.h0, self.h1, (count, 1))
        incidence = rng.uniform(.12, 1, size=count)
        incidence[:n] = rng.uniform(-.12, .12, size=n)
        angle = rng.uniform(-np.pi, np.pi, size=count)
        tangent = self.right[ids] * np.cos(angle)[:, None] + self.up[ids] * np.sin(angle)[:, None]
        ray = -self.normal[ids] * incidence[:, None] + tangent * np.sqrt(1 - incidence**2)[:, None]
        camera = point - ray * rng.uniform(.18, 32, (count, 1))
        rays = np.ascontiguousarray(np.column_stack((camera, ray)), dtype=np.float32)
        camera, ray = rays[:, :3].astype(np.float64), rays[:, 3:].astype(np.float64)
        relative = camera - self.xyz[ids, 0]

        bounds = self.extrema[ids]
        a = (bounds.min(axis=1) - 1e-5 - camera) / ray
        b = (bounds.max(axis=1) + 1e-5 - camera) / ray
        start = np.maximum(0, np.minimum(a, b).max(axis=1))
        end = np.minimum(150, np.maximum(a, b).min(axis=1))
        base = self.base[ids]
        e1, e2 = base[:, 1], base[:, 2]
        pad = 1e-5 * (np.maximum(1, np.maximum(np.linalg.norm(self.du[ids], axis=1),
                                              np.linalg.norm(self.dv[ids], axis=1)))
                      + np.linalg.norm(relative, axis=1))
        for edge in (e1, e2 - e1, -e2):
            normal = np.cross(directions.sum(axis=1), edge)
            slopes = np.einsum("ni,nji->nj", normal, directions)
            low = (np.einsum("ni,nji->nj", normal, base)
                   + np.minimum(slopes * self.h0, slopes * self.h1)).min(axis=1)
            low -= pad * np.abs(normal).sum(axis=1)
            distance, rate = dot(normal, relative), dot(normal, ray)
            crossing = (low - distance) / np.where(rate == 0, np.inf, rate)
            active = ~self.constant[ids]
            start = np.where(active & (rate > 0), np.maximum(start, crossing), start)
            end = np.where(active & (rate < 0), np.minimum(end, crossing), end)
            end = np.where(active & (rate == 0) & (distance < low), -1, end)

        origin, line = self.project(ids, directions.mean(axis=1), relative, ray)
        middle = origin[:, :2] + line[:, :2] * ((start + end) * .5)[:, None]
        weights = np.maximum(self.weights(ids, middle), 0)
        weights /= weights.sum(axis=1, keepdims=True)
        field = np.einsum("ni,nij->nj", weights, directions)
        origin, line = self.project(ids, field, relative, ray)
        z_scale = np.einsum("ni,nji->nj", self.normal[ids], directions) / dot(self.normal[ids], field)[:, None]
        slab_low = np.minimum(z_scale * self.h0, z_scale * self.h1).min(axis=1) - 1e-5
        slab_high = np.maximum(z_scale * self.h0, z_scale * self.h1).max(axis=1) + 1e-5
        vertical = np.abs(line[:, 2]) > 1e-7
        denom = np.where(vertical, line[:, 2], 1)
        a, b = (slab_low - origin[:, 2]) / denom, (slab_high - origin[:, 2]) / denom
        start = np.where(vertical, np.maximum(start, np.minimum(a, b)), start)
        end = np.where(vertical, np.minimum(end, np.maximum(a, b)), end)
        end = np.where(~vertical & ((origin[:, 2] < slab_low) | (origin[:, 2] > slab_high)), -1, end)
        span = end - start
        keep = span > 1e-7
        ids, rays, origin, line, start, span, ray = [a[keep] for a in (ids, rays, origin, line, start, span, ray)]
        assert np.isfinite(origin).all() and np.isfinite(span).all()
        teacher = np.zeros((len(ids), 2), dtype=np.float32)
        trace(self.height, *self.atlas, self.scale, self.reference, self.address,
              self.packed, len(self.xyz), rays, ids, len(ids), teacher)
        time = start[:, None] + span[:, None] * np.array([.125, .375, .625, .875])
        coords = origin[:, None, :] + line[:, None, :] * time[..., None]
        heights = self.sample(coords[..., :2] + self.uv[ids, :1])
        residual = np.clip(heights - (coords[..., 2] / self.scale + self.reference), -1.5, 1.5)
        rate = np.clip(line[:, :2] * span[:, None] * self.atlas / 16, -2, 2)
        edge_proximity = np.clip(self.weights(ids, coords[..., :2]).min(axis=-1) * 24, -1, 1)
        facing = -dot(self.normal[ids], ray)
        inputs = np.ascontiguousarray(np.column_stack((residual, rate, edge_proximity, facing)), dtype=np.float32)
        visible = teacher[:, 0] == 1
        target = np.clip((teacher[:, 1] - start) / span, 0, 1).astype(np.float32)
        known = teacher[:, 0] <= 1
        result = inputs[known], visible[known], target[known], span[known], int((~known).sum())
        if retain_rays:
            details = {"rays": rays[known], "ids": np.ascontiguousarray(ids[known]),
                       "teacher": teacher[known], "unresolved_rays": rays[~known],
                       "unresolved_ids": np.ascontiguousarray(ids[~known])}
            return result, details
        return result


def sigmoid(x):
    return 1 / (1 + np.exp(-np.clip(x, -20, 20)))


def fit(data, seed, checkpoint=None, steps=3300, checkpoint_steps=(800, 1600, 2500, 3300), sample_weights=None):
    x, visible, target, span, _ = data
    rng = np.random.default_rng(seed)
    w = [rng.normal(0, .19, (11, 16)).astype(np.float32), np.full(16, .06, np.float32),
         rng.normal(0, .09, (16, 2)).astype(np.float32), np.zeros(2, np.float32)]
    momentum, variance = [np.zeros_like(p) for p in w], [np.zeros_like(p) for p in w]
    # Increase the loss on long grazing rays using a bounded world-distance term.
    ratio = np.clip(span / .05, 1, 8).astype(np.float32)
    blend = .2 * np.clip((.30 - abs(x[:, -1])) / .18, 0, 1) * np.clip((ratio - 1) / 2, 0, 1)
    probabilities = None
    if sample_weights is not None:
        probabilities = np.asarray(sample_weights, dtype=np.float64)
        if probabilities.shape != (len(x),) or not np.isfinite(probabilities).all() or (probabilities <= 0).any():
            raise ValueError("Sample weights must be finite, positive and match the training set")
        probabilities = probabilities / probabilities.sum()
    for step in range(1, steps + 1):
        ids = rng.integers(len(x), size=1024)
        if probabilities is not None:
            ids = rng.choice(len(x), size=1024, p=probabilities)
        features = x[ids]
        active = np.maximum(features @ w[0] + w[1], 0)
        prediction = sigmoid(active**2 @ w[2] + w[3])
        seed_error = prediction[:, 0] - target[ids]
        gradient = (1 - blend[ids]) * seed_error + blend[ids] * ratio[ids] * np.clip(seed_error * ratio[ids], -.5, .5)
        err = np.column_stack((2.2 * gradient * prediction[:, 0] * (1 - prediction[:, 0]) * visible[ids],
                               .34 * (prediction[:, 1] - visible[ids]))) / len(ids)
        layer = (err @ w[2].T) * 2 * active
        gradients = (features.T @ layer, layer.sum(axis=0), (active**2).T @ err, err.sum(axis=0))
        lr = .004 if step < 2500 else .0014
        for j, grad in enumerate(gradients):
            momentum[j] = .9 * momentum[j] + .1 * grad
            variance[j] = .999 * variance[j] + .001 * grad**2
            w[j] -= lr * (momentum[j] / (1 - .9**step)) / (np.sqrt(variance[j] / (1 - .999**step)) + 1e-8)
        if checkpoint and (step in checkpoint_steps or step == steps):
            checkpoint(step, [value.copy() for value in w])
    return w


def score(data, w):
    x, hit, target, span, unresolved = data
    pred = sigmoid(np.maximum(x @ w[0] + w[1], 0)**2 @ w[2] + w[3])
    grazing = abs(x[:, -1]) < .12
    error = abs(pred[:, 0] - target) * span * 1000
    report = {"rays": len(x), "unresolved_labels_excluded": unresolved,
              "miss_rejection_at_005": float((pred[~hit, 1] < .05).mean()) if (~hit).any() else None}
    for name, mask in (("all", np.ones_like(hit)), ("grazing", grazing), ("long_grazing", grazing & (span > .10))):
        selected = hit & mask
        report[name] = {
            "rays": int(mask.sum()), "hits": int(selected.sum()),
            "seed_p50_mm": float(np.median(error[selected])) if selected.any() else None,
            "seed_p95_mm": float(np.quantile(error[selected], .95)) if selected.any() else None,
            "midpoint_p95_mm": float(np.quantile(abs(.5 - target[selected]) * span[selected] * 1000, .95)) if selected.any() else None,
            "hit_recall_at_005": float((pred[selected, 1] >= .05).mean()) if selected.any() else None,
        }
    return report


def shader_weights(models):
    def fmt(row):
        return ", ".join(f"{v:.9e}f" for v in row)

    def constant(kind, name, rows, width):
        return [f"static const {kind} {name}{width} = {{", *("    " + row + "," for row in rows), "};"]

    lines = ["// SPDX-License-Identifier: MIT", "#pragma once",
             "// Generated by Tools/train_welded_tower.py from the authored height profiles."]
    for name, w in models.items():
        lines += constant("float", name + "W0", ["{" + fmt(row) + "}" for row in w[0].T], "[16][11]")
        lines.append(f"static const float {name}B0[16] = {{" + fmt(w[1]) + "};")
        lines += constant("float2", name + "W1", ["float2(" + fmt(row) + ")" for row in w[2]], "[16]")
        lines.append(f"static const float2 {name}B1 = float2(" + fmt(w[3]) + ");")
    return "\n".join(lines) + "\n"


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("manifest", type=Path)
    cli.add_argument("teacher", type=Path)
    cli.add_argument("output", type=Path)
    cli.add_argument("--open-shaft", action="store_true", help="Train the four open-shaft tower profiles")
    args = cli.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(args.manifest.read_bytes())
    mesh_file = args.manifest.parent / manifest["sidecar"]
    mesh = json.loads(mesh_file.read_bytes())
    faces = sorted(mesh["faces"], key=lambda f: f["stable_id"])
    vertices = np.ascontiguousarray([f["vertices"] for f in faces], dtype=np.uint32)
    xyz = np.ascontiguousarray([[mesh["vertices"][str(v)] for v in face] for face in vertices], dtype=np.float32)
    uv = np.asarray([f["uv"] for f in faces])
    tagged = np.ascontiguousarray([f["region"] != 0 for f in faces], dtype=np.uint32)
    directions = np.empty_like(xyz)
    dll = ctypes.CDLL(str(args.teacher.resolve()))
    fp = np.ctypeslib.ndpointer(dtype=np.float32, flags="C_CONTIGUOUS")
    up = np.ctypeslib.ndpointer(dtype=np.uint32, flags="C_CONTIGUOUS")
    u, f = ctypes.c_uint, ctypes.c_float
    dll.BlendDirections.argtypes = [fp, up, up, u, fp]
    dll.BlendDirections.restype = ctypes.c_bool
    assert dll.BlendDirections(xyz, vertices, tagged, len(faces), directions)
    dll.TraceRays.argtypes = [fp, u, u, f, f, u, fp, u, fp, up, u, fp]
    dll.TraceRays.restype = None

    if args.open_shaft:
        names = ("OpenShaftWall", "OpenShaftStairs", "OpenShaftTrim", "OpenShaftReveal")
        header = "NeuralOpenShaftTowerWeights.azsli"
    else:
        names = ("WeldedWall", "WeldedStairs", "WeldedDoor")
        header = "NeuralWeldedTowerWeights.azsli"
    assert set(manifest["profiles"]) == {str(i) for i in range(1, len(names) + 1)}
    models, report = {}, {"surface_sha256": hashlib.sha256(mesh_file.read_bytes()).hexdigest(), "profiles": {}}
    for profile_id, name in enumerate(names, start=1):
        config = manifest["profiles"][str(profile_id)]
        height = args.manifest.parent / config["height"]
        keep = np.asarray([face["region"] and face["profile"] == profile_id for face in faces], dtype=bool)
        profile = Profile(config, height, xyz[keep], uv[keep], directions[keep])
        print(f"{name}: tracing training and held-out rays", flush=True)
        train = profile.dataset(dll.TraceRays, 230000, 12631 + profile_id)
        test = profile.dataset(dll.TraceRays, 48000, 78306 + profile_id)
        print(f"{name}: fitting {len(train[0])} labeled rays", flush=True)
        weights = fit(train, 7112026 + profile_id)
        models[name] = weights
        report["profiles"][str(profile_id)] = {
            **config, "faces": int(keep.sum()), "height_size": profile.atlas.tolist(),
            "height_sha256": hashlib.sha256(height.read_bytes()).hexdigest(),
            "training_rays": len(train[0]), "training_hits": int(train[1].sum()),
            "training_grazing_rays": int((abs(train[0][:, -1]) < .12).sum()),
            "training_unresolved_labels_excluded": train[-1], "held_out": score(test, weights),
        }
        print(json.dumps(report["profiles"][str(profile_id)]), flush=True)
        (args.output / "training.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (args.output / header).write_text(shader_weights(models), encoding="utf-8")


if __name__ == "__main__":
    main()

"""Fit switchable angular-occlusion prototypes at selected tower receivers.

Offline only: preserves production shaders, learned bundles and saved settings.
Reference labels come from the existing continuous FP64 bilinear-cell teacher.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re

import numpy as np
from PIL import Image
from directional_occlusion import (REACH_CAPS, cosine_directions, directional_field,
                                  fit_angular, metrics, normalize, query_directions)
from level_one import Evaluator, load_profiles
from level_three import Native


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def encode(values):
    values = np.asarray(values)
    packed = np.where(values < 0, 255, np.rint(np.clip(values, 0, 1) * 252)).astype(np.uint8)
    return base64.b64encode(packed.tobytes()).decode("ascii")


def select_receivers(profile, count):
    rng = np.random.default_rng(519831)
    centers = profile.xyz.mean(axis=1)
    radius = np.linalg.norm(centers[:, :2], axis=1)
    column = np.flatnonzero((radius > .8) & (radius < 1.1))
    if not len(column):
        raise ValueError("No central-column faces found; select the open-shaft wall profile")
    ids = rng.choice(column, 3000)
    bary = rng.dirichlet([3, 3, 3], len(ids))
    positions = np.einsum("ni,nij->nj", bary, profile.xyz[ids])
    keep = (positions[:, 2] > 7) & (positions[:, 2] < 13)
    ids, bary, positions = ids[keep], bary[keep], positions[keep]
    uv = np.einsum("ni,nij->nj", bary, profile.uv[ids])
    height = profile.sample(uv)
    receivers = []
    used = []
    for quantile in np.linspace(.04, .96, count):
        for i in np.argsort(abs(height - np.quantile(height, quantile))):
            q = np.mod(uv[i], 1)
            if any(np.linalg.norm((q - p) * profile.atlas) < 24 for p in used):
                continue
            used.append(q)
            face = profile.packed[ids[i]].astype(float)
            n = normalize(face[24:27])
            tangent = normalize(face[3:6] - n * np.dot(face[3:6], n))
            frame = np.array([tangent, np.cross(n, tangent), n])
            grid = np.linspace(-48, 48, 65)
            x, y = np.meshgrid(grid, grid)
            crop = profile.sample(uv[i] + np.stack([x, y], axis=-1) / profile.atlas)
            receivers.append({"name": f"Column relief {len(receivers)+1} · height {height[i]:.2f}",
                              "face": int(ids[i]), "uv": uv[i].tolist(), "height": float(height[i]),
                              "frame": frame.tolist(), "base_position": positions[i].tolist(),
                              "crop": base64.b64encode(np.rint(crop * 255).astype(np.uint8).tobytes()).decode("ascii")})
            break
    return receivers


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("manifest", type=Path)
    cli.add_argument("binary_dir", type=Path)
    cli.add_argument("output", type=Path)
    cli.add_argument("--receivers", type=int, default=6)
    cli.add_argument("--steps", type=int, default=600)
    cli.add_argument("--reuse", action="store_true")
    args = cli.parse_args()
    if not 1 <= args.receivers <= 32 or args.steps < 1:
        cli.error("Receivers must be 1-32 and fitting steps positive")
    gem = Path(__file__).resolve().parents[1]
    project = gem.parents[1]
    if args.output.resolve().is_relative_to(gem / "Assets"):
        cli.error("Experiment output must stay outside shipping assets")
    args.output.mkdir(parents=True, exist_ok=True)
    geometry = Evaluator(args.binary_dir)
    _, _, profile, _, mesh_path = next(iter(load_profiles(args.manifest, geometry)))
    height_path = project / "Assets/SilPOMPlanarAB/IrishFieldstoneTowerWelded/wall/height.png"
    profile.height = np.ascontiguousarray(np.asarray(Image.open(height_path), np.float32) / 65535)
    profile.atlas = np.asarray(profile.height.shape[::-1])
    level_path = project / "Levels/VueLiveLink/VueLiveLink.prefab"
    level = json.loads(level_path.read_bytes())
    tower = next(i for i in level["Instances"].values() if i["Source"].endswith("IrishFieldstoneTowerOpenShaft.prefab"))
    depth = 1.0
    for patch in tower["Patches"]:
        if patch["path"].endswith("/ReliefDepthMultiplier"):
            depth = float(patch["value"])
    profile.scale *= depth
    receiver_file = args.output / "receivers.json"
    receivers = json.loads(receiver_file.read_text()) if args.reuse and receiver_file.exists() else select_receivers(profile, args.receivers)
    if len(receivers) != args.receivers:
        raise ValueError("Cached receiver count differs from the requested experiment")
    receiver_file.write_text(json.dumps(receivers, indent=2))
    elevation = np.radians(np.linspace(1.5, 88.5, 24))
    azimuth = np.arange(64) * 2 * np.pi / 64
    aa, ee = np.meshgrid(azimuth, elevation)
    map_directions = np.column_stack((np.cos(ee).ravel() * np.cos(aa).ravel(),
                                     np.cos(ee).ravel() * np.sin(aa).ravel(), np.sin(ee).ravel()))
    sections = {"train": query_directions(512, 39181), "calibration": query_directions(256, 72931),
                "test": query_directions(512, 98237), "ambient": cosine_directions(256),
                "map": map_directions, "field": cosine_directions(32)}
    slices = {}; start = 0
    for key, values in sections.items():
        slices[key] = slice(start, start + len(values)); start += len(values)
    directions = np.concatenate(list(sections.values()))
    contract = {"schema": 1, "reach_caps_m": list(REACH_CAPS), "zero_reach": "relief-top exit",
                "frame": "surface-anchored base UV-u tangent, bitangent, face normal; no camera input",
                "teacher": "FP64 continuous bilinear-cell visibility; local frozen displacement direction; continued material",
                "footprints": ["point", "4-texel square / 2x2 quadrature"], "depth_multiplier": depth,
                "scale_m": profile.scale, "height_sha256": digest(height_path), "mesh_sha256": digest(mesh_path),
                "native_sha256": digest(args.binary_dir / "silpom_visibility.dll"),
                "receiver_sha256": digest(receiver_file), "direction_sha256": hashlib.sha256(directions.tobytes()).hexdigest()}
    cache = args.output / "labels.npz"; meta = args.output / "contract.json"
    if args.reuse and cache.exists():
        if not meta.exists() or json.loads(meta.read_text()) != contract:
            raise ValueError("Cached teacher contract changed")
        loaded = np.load(cache); truth, current = loaded["truth"], loaded["current"]
    else:
        native = Native(args.binary_dir)
        groups, requests, ids = [], [], []
        cursor = 0
        for point in receivers:
            world = normalize(directions @ np.asarray(point["frame"]))
            for footprint in (0, 4):
                offsets = np.array([[0., 0.]]) if footprint == 0 else np.array([[-1., -1.], [1., -1.], [-1., 1.], [1., 1.]])
                for reach in REACH_CAPS:
                    uv = np.repeat(np.asarray(point["uv"])[None] + offsets / profile.atlas, len(directions), axis=0)
                    light = np.tile(world, (len(offsets), 1))
                    q = np.column_stack((uv, light, np.full(len(uv), reach), np.full(len(uv), 16)))
                    requests.append(q); ids.append(np.full(len(q), point["face"], np.uint32))
                    groups.append((cursor, cursor + len(q), len(offsets)))
                    cursor += len(q)
        header = (gem / "Assets/Shaders/SilPOM/VisibilityClassificationThresholds.azsli").read_text()
        threshold = float(re.search(r"VisibilityWallThreshold\s*=\s*([\d.eE+\-]+)f", header).group(1))
        print("Generating", cursor, "directional teacher/classifier queries", flush=True)
        output, stats, _ = native.evaluate(profile, np.concatenate(requests), np.concatenate(ids), 4, 3, threshold)
        truth, current = [], []
        for left, right, samples in groups:
            group = output[left:right].reshape(samples, len(directions), 4)
            known = np.all(group[:, :, 2] >= 0, axis=0)
            truth.append(np.where(known, group[:, :, 2].mean(axis=0), -1))
            current.append(group[:, :, 1].mean(axis=0))
        shape = (len(receivers), 2, len(REACH_CAPS), len(directions))
        truth = np.array(truth).reshape(shape); current = np.array(current).reshape(shape)
        np.savez_compressed(cache, truth=truth, current=current)
        meta.write_text(json.dumps(contract, indent=2) + "\n")
    known_pair = (truth[:, :, 1:] >= 0) & (truth[:, :, :-1] >= 0)
    if np.any((np.diff(truth, axis=2) > 1e-6) & known_pair):
        raise ValueError("Teacher violated physical reach monotonicity")
    result = {"contract": contract, "neural_decoders_trained": False, "runtime_modified": False,
              "sources": {str(p.relative_to(gem)): digest(p) for p in (Path(__file__), gem / "Tools/directional_occlusion.py")},
              "selection": "Calibration directions only; held-out angles never select fits", "receivers": []}
    preview = {"reaches": ["25 mm", "100 mm", "300 mm", "Relief-top exit"], "receivers": receivers,
               "azimuths": np.degrees(azimuth).round(5).tolist(), "elevations": np.degrees(elevation).round(5).tolist(),
               "teacher": encode(truth[..., slices["map"]]), "current": encode(current[..., slices["map"]]),
               "field": encode(truth[..., slices["field"]]), "models": [], "metrics": []}
    for key, values in (("ambientTeacher", truth), ("ambientCurrent", current)):
        samples = values[..., slices["ambient"]]
        valid = samples >= 0
        mean = np.sum(np.where(valid, samples, 0), axis=-1) / np.maximum(1, valid.sum(axis=-1))
        preview[key] = encode(np.where(valid.any(axis=-1), mean, -1))
    for i, point in enumerate(receivers):
        entry = {"name": point["name"], "face": point["face"], "footprints": []}
        model_modes, metric_modes = [], []
        for fp in range(2):
            mode = {"models": {}, "held_out": {}}
            models, all_metrics = {}, {}
            expected = truth[i, fp, :, slices["test"]]
            candidates = {"current": current[i, fp, :, slices["test"]],
                          "field32": directional_field(truth[i, fp, :, slices["field"]], sections["test"])}
            for count in (4, 8):
                print("Fitting", i + 1, "footprint", fp, "occluders", count, flush=True)
                model, info = fit_angular(sections["train"], truth[i, fp, :, slices["train"]],
                                          sections["calibration"], truth[i, fp, :, slices["calibration"]],
                                          count, bool(fp), args.steps)
                key = "caps" + str(count)
                models[key] = model.to_dict(); mode["models"][key] = info
                candidates[key] = model.visibility(sections["test"])
                assert np.all(np.diff(model.visibility(sections["map"]), axis=0) <= 1e-10)
            for key, predicted in candidates.items():
                mode["held_out"][key] = metrics(predicted, expected, sections["test"])
                all_metrics[key] = [metrics(predicted[b:b+1], expected[b:b+1], sections["test"])
                                    for b in range(len(REACH_CAPS))]
            entry["footprints"].append(mode); model_modes.append(models); metric_modes.append(all_metrics)
        result["receivers"].append(entry); preview["models"].append(model_modes); preview["metrics"].append(metric_modes)
        (args.output / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    (args.output / "preview.json").write_text(json.dumps(preview, separators=(",", ":")))
    print("Complete:", args.output, flush=True)


if __name__ == "__main__":
    main()

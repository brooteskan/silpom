"""Evaluate and select small-network checkpoints by completed solver outcomes.

Outputs are experiments only: this tool never installs weights or edits assets.
Requires the standalone CMake silpom_level_one and silpom_welded_teacher DLLs.
"""
import argparse
import ctypes
import hashlib
import json
import os
import re
from pathlib import Path
os.environ.setdefault("OPENBLAS_NUM_THREADS", "2")
import numpy as np
from train_welded_tower import Profile, fit, score, shader_weights
from level_one_reference import trace

HERE = Path(__file__).resolve().parent
FP = np.ctypeslib.ndpointer(dtype=np.float32, flags="C_CONTIGUOUS")
UP = np.ctypeslib.ndpointer(dtype=np.uint32, flags="C_CONTIGUOUS")
U, F = ctypes.c_uint, ctypes.c_float
BUDGETS = ((1, 1, 5.), (2, 1, 5.), (4, 1, 1.), (6, 4, 1.))


def load_weights(header, name):
    text = header.read_text()
    arrays = []
    for suffix, shape in (("W0", (16, 11)), ("B0", (16,)), ("W1", (16, 2)), ("B1", (2,))):
        match = re.search(r"\b" + name + suffix + r"(?:\[[^\]]*\])*\s*=\s*(.*?);", text, re.S)
        if not match:
            raise ValueError(f"Missing {name + suffix} in {header}")
        values = re.findall(r"[-+]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?f\b", match[1])
        arrays.append(np.asarray([float(v[:-1]) for v in values], dtype=np.float32).reshape(shape))
    return [arrays[0].T.copy(), arrays[1], arrays[2], arrays[3]]


def packed_weights(weights):
    return np.ascontiguousarray(np.concatenate((weights[0].T.ravel(), weights[1],
                                                weights[2].ravel(), weights[3])), dtype=np.float32)


class Evaluator:
    def __init__(self, binary_dir):
        self.teacher = ctypes.CDLL(str((binary_dir / "silpom_welded_teacher.dll").resolve()))
        self.teacher.BlendDirections.argtypes = [FP, UP, UP, U, FP]
        self.teacher.BlendDirections.restype = ctypes.c_bool
        self.teacher.TraceRays.argtypes = [FP, U, U, F, F, U, FP, U, FP, UP, U, FP]
        self.teacher.TraceRays.restype = None
        self.native = ctypes.CDLL(str((binary_dir / "silpom_level_one.dll").resolve()))
        self.native.EvaluateLevelOne.argtypes = [FP, U, U, F, F, U, FP, U, FP, UP, U, U,
                                                U, U, F, ctypes.POINTER(F), FP, UP]
        self.native.EvaluateLevelOne.restype = ctypes.c_bool

    def evaluate(self, profile, details, model, budget, weights=None):
        rays, ids = np.ascontiguousarray(details["rays"]), np.ascontiguousarray(details["ids"])
        hits, stats = np.zeros((len(ids), 5), np.float32), np.zeros((len(ids), 16), np.uint32)
        packed = packed_weights(weights) if weights is not None else None
        pointer = packed.ctypes.data_as(ctypes.POINTER(F)) if packed is not None else None
        valid = self.native.EvaluateLevelOne(profile.height, *profile.atlas, profile.scale, profile.reference,
                                             profile.address, profile.packed, len(profile.packed), rays, ids,
                                             len(ids), model, *budget, pointer, hits, stats)
        if not valid:
            raise ValueError("Native evaluator rejected its arguments")
        return hits, stats


def outcomes(data, details, hits, stats, depth_tolerance_m=.00025):
    teacher_hit = data[1]
    final_hit, fill = hits[:, 0] == 1, stats[:, 7] > 0
    # Geometric recovery can succeed after every neural candidate failed.
    # All accepted non-fill hits have passed surface validation.
    refined = final_hit & ~fill
    errors = np.abs(hits[:, 1] - details["teacher"][:, 1])
    comparable = refined & teacher_hit
    summarize = {
        "rays": len(hits), "teacher_hits": int(teacher_hit.sum()),
        "primary_successes": int(stats[:, 2].sum()), "retry_successes": int(stats[:, 3].sum()),
        "recovered_after_predictor_miss": int((refined & (stats[:, 2:4].sum(axis=1) == 0)).sum()),
        "rays_with_retries": int((stats[:, 4] > 0).sum()), "retries": int(stats[:, 4].sum()),
        "confidence_rejections": int(stats[:, 5].sum()), "fallback_fills": int(fill.sum()),
        "uncovered_teacher_hits": int((teacher_hit & ~final_hit).sum()),
        "teacher_hits_using_fill": int((teacher_hit & fill).sum()),
        "refined_false_positives": int((refined & ~teacher_hit).sum()),
        "refined_depth_disagreements": int((comparable & (errors > depth_tolerance_m)).sum()),
        "depth_comparison_tolerance_mm": depth_tolerance_m * 1000,
        "refined_depth_p95_mm": float(np.quantile(errors[comparable], .95) * 1000) if comparable.any() else None,
        "refined_depth_p99_mm": float(np.quantile(errors[comparable], .99) * 1000) if comparable.any() else None,
        "refined_depth_max_mm": float(errors[comparable].max() * 1000) if comparable.any() else None,
        "mean_iterations_per_ray": float(stats[:, 0].mean()),
        "total_iterations": int(stats[:, 0].sum()),
        "iterations_histogram": {str(i): int(n) for i, n in zip(*np.unique(stats[:, 0], return_counts=True))},
        "candidate_histogram": {str(i): int(n) for i, n in zip(*np.unique(stats[:, 1], return_counts=True))},
        "rejection_reasons": dict(zip(("outside_or_interval", "singular_jacobian", "escaped_iterate", "budget"),
                                     [int(n) for n in stats[:, 8:12].sum(axis=0)])),
    }
    return summarize


QUALITY_KEYS = ("uncovered_teacher_hits", "teacher_hits_using_fill", "refined_false_positives",
                "refined_depth_disagreements")


def no_quality_regression(candidate, baseline):
    if not all(candidate[key] <= baseline[key] for key in QUALITY_KEYS):
        return False
    for key in ("refined_depth_p95_mm", "refined_depth_p99_mm", "refined_depth_max_mm"):
        value, reference = candidate.get(key), baseline.get(key)
        # Metrics are millimetres. A 1e-6 mm allowance was below FP32 ray-distance
        # resolution and rejected numerically equivalent tails.
        allowance=max(.01,abs(reference or 0)*1e-5)
        if value is not None and reference is not None and value > reference + allowance:
            return False
    return True


def checkpoint_rank(summary):
    return (sum(summary[key] for key in QUALITY_KEYS), summary["total_iterations"], summary["retries"])


def load_profiles(manifest_path, evaluator, open_shaft=True, height_root=None):
    manifest = json.loads(manifest_path.read_bytes())
    mesh_path = manifest_path.parent / manifest["sidecar"]
    mesh = json.loads(mesh_path.read_bytes())
    faces = sorted(mesh["faces"], key=lambda face: face["stable_id"])
    vertices = np.ascontiguousarray([face["vertices"] for face in faces], dtype=np.uint32)
    xyz = np.ascontiguousarray([[mesh["vertices"][str(v)] for v in face] for face in vertices], dtype=np.float32)
    uv = np.asarray([face["uv"] for face in faces])
    tagged = np.ascontiguousarray([face["region"] != 0 for face in faces], dtype=np.uint32)
    directions = np.empty_like(xyz)
    if not evaluator.teacher.BlendDirections(xyz, vertices, tagged, len(faces), directions):
        raise ValueError("Invalid blended face topology")
    names = ("OpenShaftWall", "OpenShaftStairs", "OpenShaftTrim", "OpenShaftReveal") if open_shaft else (
        "WeldedWall", "WeldedStairs", "WeldedDoor")
    for ident, name in enumerate(names, 1):
        config = manifest["profiles"][str(ident)]
        height_path = (height_root or manifest_path.parent) / config["height"]
        keep = np.asarray([face["region"] and face["profile"] == ident for face in faces], dtype=bool)
        profile = Profile(config, height_path, xyz[keep], uv[keep], directions[keep])
        yield ident, name, profile, height_path, mesh_path


def audit(profile, details, hits, stats, count):
    if not count:
        return []
    incidence = np.abs(np.einsum("ij,ij->i", profile.normal[details["ids"]], details["rays"][:, 3:]))
    refined = (hits[:, 0] == 1) & (stats[:, 7] == 0)
    teacher_hit = details["teacher"][:, 0] == 1
    errors = np.where(refined & teacher_hit, np.abs(hits[:, 1] - details["teacher"][:, 1]), -1)
    groups = [np.argsort(-errors)[errors[np.argsort(-errors)] > .00025],
              np.flatnonzero(refined & ~teacher_hit),
              np.flatnonzero(stats[:, 7] > 0),
              np.flatnonzero(stats[:, 3] > 0),
              np.flatnonzero(teacher_hit & (hits[:, 0] != 1)),
              np.argsort(incidence)]
    # Round-robin strata prevent ordinary misses from crowding out wrong roots,
    # fills and successful retries. Include difficult grazing rays independently.
    choices = []
    for offset in range(max(map(len, groups), default=0)):
        for group in groups:
            if offset < len(group) and int(group[offset]) not in choices:
                choices.append(int(group[offset]))
        if len(choices) >= count:
            break
    choices = choices[:count]
    result = []
    for i in choices:
        checked = trace(profile.height, profile.packed[details["ids"][i]], details["rays"][i],
                        profile.scale, profile.reference, profile.address)
        classification = "unresolved"
        if checked["status"] == "checked":
            if stats[i, 7]:
                classification = "base_fill"
            elif refined[i]:
                classification = "spurious_refined_hit" if checked["nearest_t"] is None else (
                    "correct_refined_hit" if abs(hits[i, 1] - checked["nearest_t"]) <= .00025 else "wrong_root_or_depth")
            else:
                classification = "correct_miss" if checked["nearest_t"] is None else "missed_surface"
        result.append({"ray": int(i), "face": int(details["ids"][i]), "classification": classification,
                       "ray_origin_direction": details["rays"][i].tolist(),
                       "packed_face": profile.packed[details["ids"][i]].tolist(),
                       "shader_cpu_hit": bool(hits[i, 0] == 1), "shader_cpu_t": float(hits[i, 1]),
                       "teacher_hit": bool(details["teacher"][i, 0] == 1),
                       "teacher_t": float(details["teacher"][i, 1]), **checked})
    return result


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("manifest", type=Path)
    cli.add_argument("binary_dir", type=Path)
    cli.add_argument("output", type=Path)
    cli.add_argument("--welded", action="store_true")
    cli.add_argument("--height-root", type=Path, help="Resolve manifest height paths from another exported directory")
    cli.add_argument("--profiles", type=int, nargs="+")
    cli.add_argument("--rays", type=int, default=4000)
    cli.add_argument("--audit-rays", type=int, default=12)
    cli.add_argument("--train", action="store_true", help="Write candidate checkpoints; never install them")
    cli.add_argument("--training-rays", type=int, default=230000)
    cli.add_argument("--selection-rays", type=int, default=4000)
    cli.add_argument("--steps", type=int, default=3300)
    args = cli.parse_args()
    args.profiles = args.profiles or list(range(1, 4 if args.welded else 5))
    if any(ident not in range(1, 4 if args.welded else 5) for ident in args.profiles):
        cli.error("Profile id is not part of the selected tower model")
    if min(args.rays, args.selection_rays, args.training_rays, args.steps) < 1 or args.audit_rays < 0:
        cli.error("Ray counts/steps must be positive and audit count nonnegative")
    args.output.mkdir(parents=True, exist_ok=True)
    if args.output.resolve().is_relative_to((HERE.parent / "Assets").resolve()):
        cli.error("Experiment output must be outside shipping shader assets")
    evaluator = Evaluator(args.binary_dir)
    report = {"schema": 1, "backend": "CPU shared shader solver; ideal FP32 bilinear descriptor sampling",
              "reference": "existing CPU teacher plus independent FP64 cell-polynomial audit",
              "selection_budget": [2, 1, 5], "profiles": {}}
    header = HERE.parent / "Assets/Shaders/SilPOM" / (
        "NeuralWeldedTowerWeights.azsli" if args.welded else "NeuralOpenShaftTowerWeights.azsli")
    report["shipping_weights_sha256"] = hashlib.sha256(header.read_bytes()).hexdigest()
    report["solver_source_sha256"] = hashlib.sha256((HERE.parent / "Assets/Shaders/SilPOM/BlendedFace.azsli").read_bytes()).hexdigest()
    report["native_evaluator_sha256"] = hashlib.sha256((args.binary_dir / "silpom_level_one.dll").read_bytes()).hexdigest()
    for ident, name, profile, height_path, mesh_path in load_profiles(
            args.manifest, evaluator, not args.welded, args.height_root):
        if ident not in args.profiles:
            continue
        model = ident if args.welded else ident + 3
        print(name + ": tracing held-out evaluation rays", flush=True)
        test, details = profile.dataset(evaluator.teacher.TraceRays, args.rays, 78306 + ident, retain_rays=True)
        current = load_weights(header, name)
        entry = {"height_sha256": hashlib.sha256(height_path.read_bytes()).hexdigest(),
                 "surface_sha256": hashlib.sha256(mesh_path.read_bytes()).hexdigest(),
                 "seed_metrics": score(test, current), "budgets": {}, "checkpoints": []}
        entry["held_out_seed"] = 78306 + ident
        entry["selection_seed"] = 192877 + ident
        entry["training_seed"] = 12631 + ident
        report["profiles"][name] = entry
        for budget in BUDGETS:
            hits, stats = evaluator.evaluate(profile, details, model, budget)
            entry["budgets"][str(budget)] = outcomes(test, details, hits, stats)
            if budget == (2, 1, 5.):
                entry["independent_audit"] = audit(profile, details, hits, stats, args.audit_rays)
        if args.train:
            selection, selection_details = profile.dataset(evaluator.teacher.TraceRays, args.selection_rays,
                                                           192877 + ident, retain_rays=True)
            base_hits, base_stats = evaluator.evaluate(profile, selection_details, model, (2, 1, 5.))
            base = outcomes(selection, selection_details, base_hits, base_stats)
            best_rank, selected = checkpoint_rank(base), None
            train, train_details = profile.dataset(evaluator.teacher.TraceRays, args.training_rays,
                                                   12631 + ident, retain_rays=True)
            training_hits, training_stats = evaluator.evaluate(profile, train_details, model, (2, 1, 5.))
            refined = training_stats[:, 2:4].sum(axis=1) > 0
            depth_bad = np.abs(training_hits[:, 1] - train_details["teacher"][:, 1]) > .00025
            hard = (((training_hits[:, 0] != 1) & train[1]) | (training_stats[:, 7] > 0)
                    | (refined & (~train[1] | depth_bad)))
            sampling = np.where(hard, 3., 1.)
            entry["hard_training_examples"] = int(hard.sum())
            def checkpoint(step, weights):
                nonlocal best_rank, selected
                checkpoint_hits, checkpoint_stats = evaluator.evaluate(profile, selection_details, model, (2, 1, 5.), weights)
                summary = outcomes(selection, selection_details, checkpoint_hits, checkpoint_stats)
                eligible = no_quality_regression(summary, base)
                entry["checkpoints"].append({"step": step, "selection": summary, "eligible": eligible})
                folder = args.output / name / f"step-{step}"
                folder.mkdir(parents=True, exist_ok=True)
                (folder / "weights.azsli").write_text(shader_weights({name: weights}))
                if eligible and checkpoint_rank(summary) < best_rank:
                    best_rank, selected = checkpoint_rank(summary), [v.copy() for v in weights]
                print(name, step, "iterations", summary["total_iterations"], "eligible", eligible, flush=True)
            fit(train, 7112026 + ident, checkpoint=checkpoint, steps=args.steps, sample_weights=sampling)
            entry["selected"] = "shipping baseline"
            entry["selection_baseline"] = base
            if selected is not None:
                final_hits, final_stats = evaluator.evaluate(profile, details, model, (2, 1, 5.), selected)
                final = outcomes(test, details, final_hits, final_stats)
                entry["selected_held_out"] = final
                if no_quality_regression(final, entry["budgets"][str((2, 1, 5.))]):
                    (args.output / name / "selected-weights.azsli").write_text(shader_weights({name: selected}))
                    entry["selected"] = "candidate; requires independent audit and GPU/visual validation before installation"
                else:
                    entry["selected"] = "shipping baseline; candidate regressed on untouched held-out rays"
        (args.output / "evaluation.json").write_text(json.dumps(report, indent=2) + "\n")
        print(name, json.dumps(entry["budgets"][str((2, 1, 5.))]), flush=True)


if __name__ == "__main__":
    main()

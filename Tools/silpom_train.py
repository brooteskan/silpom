"""Train a surface profile for one profile ID of a SilPOM surface.

  python silpom_train.py --surface SURFACE.silpom.json --profile-id ID
      --height HEIGHT.png --scale METRES --reference R --address repeat|clamp
      --role Surface|Wall|Opening --networks model0|hit|full
      --tools TOOLS_DIR --profile OUT.silpomprofile [--work DIR] [--quick]

The rays are cast at the surface's own faces tagged with the profile ID (its
Blender export), over its height image at its calibration, and labelled by
SilPOM's exact CPU intersection (silpom_welded_teacher). TOOLS_DIR holds the
standalone build's silpom_welded_teacher, silpom_level_one and
silpom_visibility libraries.

Networks:
  model0  model 0's seed network, no-march hit network and, for a 6656 x 6144
          image, its seed cache (train_model_zero.py)
  hit     the hit network (16 hidden units, checkpoints ranked by solver outcome)
  full    the hit network, Level 2 and both visibility networks

Writes, in the work folder (default: <profile stem>.training next to the
profile): manifest.json, networks/*.azsli (Tools/CompiledNetworks symbol
names), visuals/*.png (what the networks were trained on), report.json and
progress lines on stdout ("PROGRESS <0-100> <text>"). Then writes the profile,
naming that networks folder, and its weights with silpom_profile.py.
"""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

os.environ.setdefault("OPENBLAS_NUM_THREADS", "2")
import numpy as np
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import silpom_profile  # noqa: E402

# The shader model (and so the Tools/CompiledNetworks symbol names) used for
# each role and network set. The role itself is the profile's; the model number
# only selects the network layout and, for the reference comparison, the
# reference networks closest to it.
SLOTS = {("hit", "Wall"): 1, ("hit", "Surface"): 2, ("hit", "Opening"): 3, ("full", "Wall"): 4,
         ("full", "Surface"): 5, ("full", "Opening"): 6, ("model0", "Surface"): 0}
PRESETS = {"full": dict(rays=230000, held=48000, selection=4000, steps=3300, l2_steps=4500,
                        vis_rays=300000, vis_steps=6500, phys_rays=100000, phys_held=20000),
           "quick": dict(rays=40000, held=8000, selection=2000, steps=1200, l2_steps=1200,
                         vis_rays=40000, vis_steps=1500, phys_rays=20000, phys_held=4000)}


def progress(percent, text):
    print(f"PROGRESS {int(percent)} {text}", flush=True)


def load_height(path):
    """Normalized height values, whatever the PNG's bit depth."""
    image = Image.open(path)
    scale = {"I;16": 65535.0, "I;16B": 65535.0, "I": 65535.0, "L": 255.0, "F": 1.0}.get(image.mode)
    if scale is None:
        image, scale = image.convert("L"), 255.0
    return np.ascontiguousarray(np.asarray(image, np.float32) / scale)


# ---------------------------------------------------------------- visuals

def downsample(height, limit=768):
    step = max(1, math.ceil(max(height.shape) / limit))
    return height[::step, ::step], step


def hillshade(profile, height):
    """The relief lit from the upper left, at its true depth."""
    small, step = downsample(height)
    texel = float(np.median(np.linalg.norm(profile.du, axis=1))) / profile.atlas[0] * step
    z = small * profile.scale
    gy, gx = np.gradient(z, max(texel, 1e-9))
    normal = np.dstack((-gx, -gy, np.ones_like(z)))
    normal /= np.linalg.norm(normal, axis=2, keepdims=True)
    light = np.array([-.5, -.6, .62]) / np.linalg.norm([-.5, -.6, .62])
    shade = np.clip(normal @ light, 0, 1) * .8 + .2 * small
    return Image.fromarray((np.clip(shade, 0, 1) * 255).astype(np.uint8)).convert("RGB")


def to_pixels(uv, image):
    return np.column_stack((uv[:, 0] * image.width, uv[:, 1] * image.height))


def hit_uvs(profile, details):
    """Atlas UV of each labelled hit, on its face's base plane."""
    hit = details["teacher"][:, 0] == 1
    ids, rays, t = details["ids"][hit], details["rays"][hit].astype(np.float64), details["teacher"][hit, 1]
    point = rays[:, :3] + rays[:, 3:] * t[:, None]
    base = profile.xyz.astype(np.float64)[ids]
    e1, e2, d = base[:, 1] - base[:, 0], base[:, 2] - base[:, 0], point - base[:, 0]
    a11, a12, a22 = (e1 * e1).sum(1), (e1 * e2).sum(1), (e2 * e2).sum(1)
    b1, b2 = (d * e1).sum(1), (d * e2).sum(1)
    det = np.where(abs(a11 * a22 - a12 * a12) < 1e-30, 1e-30, a11 * a22 - a12 * a12)
    s, r = (a22 * b1 - a12 * b2) / det, (a11 * b2 - a12 * b1) / det
    bary = np.column_stack((1 - s - r, s, r))
    return hit, np.einsum("ni,nij->nj", bary, profile.uv[ids].reshape(-1, 3, 2))


def draw_faces(image, profile, color):
    draw = ImageDraw.Draw(image)
    for tri in profile.uv.reshape(-1, 3, 2):
        draw.polygon([tuple(p) for p in to_pixels(tri, image)], outline=color)
    return image


def label(image, text):
    draw = ImageDraw.Draw(image)
    draw.rectangle((0, 0, image.width, 18), fill=(0, 0, 0))
    draw.text((6, 3), text, fill=(255, 255, 255))
    return image


def render_geometry(path, xyz, keep, size=640):
    """The whole surface from a three-quarter view, this profile's faces in orange."""
    centre = xyz.reshape(-1, 3).mean(0)
    normal = np.cross(xyz[:, 1] - xyz[:, 0], xyz[:, 2] - xyz[:, 0])
    area = np.linalg.norm(normal, axis=1)
    facing = (normal[keep].sum(0) if keep.any() else normal.sum(0))
    # Look at the tagged faces from their average normal, raised and turned a little.
    view = facing / max(np.linalg.norm(facing), 1e-9)
    up = np.array([0, 0, 1.0]) if abs(view[2]) < .9 else np.array([0, 1.0, 0])
    side = np.cross(up, view); side /= np.linalg.norm(side)
    view = view * .8 + side * .35 + up * .45; view /= np.linalg.norm(view)
    right = np.cross(up, view); right /= np.linalg.norm(right)
    top = np.cross(view, right)
    points = (xyz - centre) @ np.stack((right, top, view), 1)
    extent = max(np.abs(points[..., :2]).max(), 1e-9)
    screen = points[..., :2] / extent * (size / 2 - 16) * np.array([1, -1]) + size / 2
    unit = normal / np.maximum(area, 1e-12)[:, None]
    light = (view * .6 + top * .6 + right * .3); light /= np.linalg.norm(light)
    shade = .35 + .65 * abs(unit @ light)
    image = Image.new("RGB", (size, size), (28, 30, 34))
    draw = ImageDraw.Draw(image)
    for i in np.argsort(points[..., 2].mean(1)):  # far to near
        base = np.array([255, 150, 40]) if keep[i] else np.array([150, 150, 155])
        colour = tuple(int(c) for c in base * shade[i])
        draw.polygon([tuple(p) for p in screen[i]], fill=colour)
    label(image, f"The surface; {int(keep.sum())} faces of this profile in orange").save(path)


def write_visuals(folder, profile, height, details, faces, surface=None):
    folder.mkdir(parents=True, exist_ok=True)
    if surface is not None:
        render_geometry(folder / "geometry.png", *surface)
    relief = hillshade(profile, height)
    label(draw_faces(relief.copy(), profile, (0, 220, 255)),
          f"Relief and the {faces} faces tagged with this profile").save(folder / "surface.png")
    # Where the training rays hit: density over the relief.
    hit, uv = hit_uvs(profile, details)
    density = np.zeros((relief.height, relief.width), np.float32)
    px = np.clip(to_pixels(np.mod(uv, 1.0) if profile.address == 0 else np.clip(uv, 0, 1), relief).astype(int),
                 0, [relief.width - 1, relief.height - 1])
    np.add.at(density, (px[:, 1], px[:, 0]), 1)
    if density.max() > 0:
        from PIL import ImageFilter
        blurred = np.asarray(Image.fromarray((density / density.max() * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(3)), np.float32)
        heat = np.clip(blurred / max(blurred.max(), 1) * 1.6, 0, 1)
        base = np.asarray(relief, np.float32) * .55
        colour = np.dstack((255 * heat, 160 * heat, 40 * heat))
        rays = Image.fromarray(np.clip(base + colour, 0, 255).astype(np.uint8))
    else:
        rays = relief.copy()
    label(rays, f"Where {int(hit.sum())} training rays hit the relief").save(folder / "rays.png")
    # View directions in each face's tangent frame: centre = head-on, rim = grazing.
    size = 320
    ids, ray = details["ids"], details["rays"][:, 3:]
    cos = np.clip(-(profile.normal[ids] * ray).sum(1), -1, 1)
    azimuth = np.arctan2((profile.up[ids] * ray).sum(1), (profile.right[ids] * ray).sum(1))
    radius = np.arccos(np.clip(abs(cos), 0, 1)) / (math.pi / 2) * (size / 2 - 12)
    x = (size / 2 + radius * np.cos(azimuth)).astype(int)
    y = (size / 2 + radius * np.sin(azimuth)).astype(int)
    polar = np.zeros((size, size), np.float32)
    np.add.at(polar, (np.clip(y, 0, size - 1), np.clip(x, 0, size - 1)), 1)
    polar = np.log1p(polar) / max(np.log1p(polar.max()), 1e-6)
    angles = Image.fromarray((np.dstack((polar * 140, polar * 230, polar * 255))).astype(np.uint8))
    draw = ImageDraw.Draw(angles)
    for fraction in (30 / 90, 60 / 90, 1.0):
        r = (size / 2 - 12) * fraction
        draw.ellipse((size / 2 - r, size / 2 - r, size / 2 + r, size / 2 + r), outline=(90, 90, 90))
    grazing = float((abs(cos) < .12).mean())
    draw.text((size / 2 + 3, size / 2 + 3), "head-on", fill=(160, 160, 160))
    draw.text((size / 2 + 3, 26), "grazing", fill=(160, 160, 160))
    label(angles, f"View directions: rings at 30, 60, 90 deg; {grazing:.0%} grazing").save(folder / "angles.png")


def write_errors(folder, name, profile, height, details, data, weights, title):
    """Seed error of a hit network at each held-out hit: green < 1 mm, yellow < 5 mm, red beyond."""
    from train_welded_tower import sigmoid
    x, hit, target, span, _ = data
    prediction = sigmoid(np.maximum(x @ weights[0] + weights[1], 0) ** 2 @ weights[2] + weights[3])
    error = abs(prediction[:, 0] - target) * span * 1000
    _, uv = hit_uvs(profile, details)
    image = hillshade(profile, height)
    image = Image.fromarray((np.asarray(image, np.float32) * .6).astype(np.uint8))
    draw = ImageDraw.Draw(image)
    pixels = to_pixels(np.mod(uv, 1.0) if profile.address == 0 else np.clip(uv, 0, 1), image)
    for (px, py), e in zip(pixels, error[hit]):
        colour = (60, 230, 90) if e < 1 else (250, 215, 50) if e < 5 else (250, 60, 50)
        draw.ellipse((px - 1.5, py - 1.5, px + 1.5, py + 1.5), fill=colour)
    good = float((error[hit] < 1).mean()) if hit.any() else 0
    label(image, f"{title}: {good:.0%} of held-out hits seeded within 1 mm").save(folder / name)
    return good


# ---------------------------------------------------------------- training

def surface_profile(args, work, teacher):
    """The profile ID's faces, directions blended over the whole surface."""
    from train_model_zero import load_surface
    from train_welded_tower import Profile
    manifest = {"sidecar": os.path.relpath(args.surface, work).replace("\\", "/"),
                "profiles": {str(args.profile_id): {
                    "height": os.path.relpath(args.height, work).replace("\\", "/"), "scale_metres": args.scale,
                    "reference": args.reference, "address": args.address, "tiling": [1, 1], "offset": [0, 0],
                    "surface": f"profile {args.profile_id} of {args.surface.name}"}}}
    (work / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    _, _, faces, xyz, uv, directions, dll = load_surface(work / "manifest.json", teacher)
    keep = np.asarray([f["region"] and f["profile"] == args.profile_id for f in faces], dtype=bool)
    if not keep.any():
        raise SystemExit(f"No faces of {args.surface.name} are tagged with profile {args.profile_id}")
    config = dict(manifest["profiles"][str(args.profile_id)])
    profile = Profile(config, args.height, xyz[keep], uv[keep], directions[keep])
    profile.height = load_height(args.height)
    profile.atlas = np.array(profile.height.shape[::-1])
    profile.xyz = xyz[keep]
    profile.surface = (xyz, keep)
    return profile, dll, int(keep.sum())


def train_hit(args, preset, profile, dll, slot, networks, report):
    from level_one import Evaluator, outcomes, checkpoint_rank
    from train_welded_tower import fit, score, shader_weights
    name = silpom_profile.MODELS[slot][1]
    evaluator = Evaluator(args.tools)
    progress(12, "Tracing training rays with the exact CPU intersection")
    train, train_details = profile.dataset(dll.TraceRays, preset["rays"], 12631, retain_rays=True)
    test, test_details = profile.dataset(dll.TraceRays, preset["held"], 78306, retain_rays=True)
    selection, selection_details = profile.dataset(dll.TraceRays, preset["selection"], 192877, retain_rays=True)
    write_visuals(args.work / "visuals", profile, profile.height, train_details, report["faces"], profile.surface)
    # Checkpoints are chosen, and results measured, with the solver budget the
    # mesh renders with (validation iterations, retries, tolerance multiplier).
    budget = args.budget
    steps, rounds = preset["steps"], args.rounds
    # Each round refits with extra weight on the training rays the previous
    # round's network left to the solver to miss, fill or get wrong.
    sampling = np.ones(len(train[0]))
    candidates, history = [], []
    for round_index in range(rounds):
        def checkpoint(step, weights):
            hits, stats = evaluator.evaluate(profile, selection_details, slot, budget, weights)
            summary = outcomes(selection, selection_details, hits, stats)
            candidates.append((checkpoint_rank(summary), round_index, step, [w.copy() for w in weights], summary))
            done = (round_index + step / steps) / rounds
            progress(15 + 30 * done, f"Hit network round {round_index + 1}/{rounds}, step {step}: "
                     f"{summary['uncovered_teacher_hits']} of {summary['teacher_hits']} selection hits uncovered")
        fit(train, 7112026 + round_index, checkpoint=checkpoint, steps=steps, sample_weights=sampling,
            checkpoint_steps=tuple(sorted({max(1, steps * k // 4) for k in (1, 2, 3, 4)})))
        best = min((c for c in candidates if c[1] == round_index), key=lambda c: c[0])
        if round_index + 1 < rounds:
            hits, stats = evaluator.evaluate(profile, train_details, slot, budget, best[3])
            teacher_hit = train[1]
            refined = stats[:, 2:4].sum(axis=1) > 0
            depth_bad = np.abs(hits[:, 1] - train_details["teacher"][:, 1]) > .00025
            hard = (((hits[:, 0] != 1) & teacher_hit) | (stats[:, 7] > 0) | (refined & (~teacher_hit | depth_bad)))
            sampling = sampling + 2.0 * hard
            history.append({"round": round_index + 1, "selection": best[4], "hard_training_rays": int(hard.sum())})
        else:
            history.append({"round": round_index + 1, "selection": best[4]})
    rank, chosen_round, step, weights, _ = min(candidates, key=lambda c: c[0])
    hits, stats = evaluator.evaluate(profile, test_details, slot, budget, weights)
    reference_hits, reference_stats = evaluator.evaluate(profile, test_details, slot, budget)
    report["solver_budget"] = list(budget)
    report["hit_network"] = {"selected_round": chosen_round + 1, "selected_step": step, "rounds": history,
                             "held_out": score(test, weights),
                             "solver_held_out": outcomes(test, test_details, hits, stats)}
    report["hit_network"]["within_1mm"] = write_errors(args.work / "visuals", "errors.png", profile, profile.height,
                                                       test_details, test, weights, "Trained hit network")
    # The reference networks of the same role, on this surface, for comparison.
    from level_one import load_weights
    reference = load_weights(silpom_profile.SHADERS / silpom_profile.MODELS[slot][0], name)
    report["reference_hit_network"] = {"model": slot, "held_out": score(test, reference),
                                       "solver_held_out": outcomes(test, test_details, reference_hits, reference_stats)}
    report["reference_hit_network"]["within_1mm"] = write_errors(
        args.work / "visuals", "errors-reference.png", profile, profile.height, test_details, test, reference,
        "Reference network (another image)")
    (networks / "HitNetwork.azsli").write_text(shader_weights({name: weights}))
    return name


def train_level_two(args, preset, profile, slot, networks, report):
    import level_two
    from level_one import outcomes
    short = silpom_profile.MODELS[slot][2]
    native = level_two.Native(args.tools)
    progress(48, "Level 2: generating native descriptors and labels")
    train, train_details, valid, _ = level_two.dataset(native, profile, slot, preset["rays"], 12631)
    selection, selection_details, _, _ = level_two.dataset(native, profile, slot, preset["selection"], 192877)
    train = tuple(v[valid] if isinstance(v, np.ndarray) else v for v in train)
    train_details = {k: np.ascontiguousarray(v[valid]) for k, v in train_details.items() if k in ("ids", "rays", "teacher")}
    cal = min(16384, len(train[0]))
    cal_data = tuple(v[:cal] if isinstance(v, np.ndarray) else v for v in train)
    cal_details = {k: np.ascontiguousarray(v[:cal]) for k, v in train_details.items()}
    candidates = []
    def checkpoint(step, weights):
        level_two.calibrate(native, profile, slot, cal_data, cal_details, weights)
        hits, stats = native.evaluate2(profile, selection_details, slot, args.budget, weights)
        summary = outcomes(selection, selection_details, hits, stats)
        candidates.append((level_two.quality_rank(summary), step, [w.copy() for w in weights], summary))
        progress(48 + 17 * step / preset["l2_steps"], f"Level 2 step {step}")
    level_two.fit(train, preset["l2_steps"], checkpoint, 7112026)
    _, step, weights, summary = min(candidates, key=lambda c: c[0])
    report["level_two"] = {"selected_step": step, "selection": summary}
    (networks / "LevelTwo.azsli").write_text(level_two.shader({short: weights}))


def train_visibility(args, preset, profile, slot, networks, report):
    import level_three
    from evaluate_visibility_only import classification, threshold
    short = silpom_profile.MODELS[slot][2]
    native = level_three.Native(args.tools)
    progress(66, "Visibility: tracing reference visibility")
    q, ids = level_three.requests(profile, preset["vis_rays"], 39913)
    o, s, x = native.evaluate(profile, q, ids, slot)
    valid = (s[:, 10] > 0) & (o[:, 2] >= 0)
    if valid.sum() < 64:
        raise SystemExit("Too few usable visibility descriptors on this surface")
    progress(72, "Visibility: fitting")
    w = level_three.fit(x[valid], o[valid, 2], preset["vis_steps"], 7112031)
    (networks / "Visibility.azsli").write_text(level_three.shader({short: w}))
    report["visibility"] = {"training_descriptors": int(valid.sum())}
    # Physical reach v2, at the calibrated relief depth multiplier 0.5.
    progress(78, "Physical visibility: tracing reference visibility")
    scale = profile.scale
    profile.scale = scale * .5
    try:
        sets = []
        for count, seed in ((preset["phys_rays"], 39913), (preset["phys_held"], 65239), (preset["phys_held"], 98239)):
            q, ids = level_three.requests(profile, count, seed)
            rng = np.random.default_rng(seed + 101)
            q[:, 6] = rng.choice([5, 9, 16, 18, 32, 64], count, p=[.05, .2, .4, .1, .1, .15])
            q[:, 5] = np.minimum(q[:, 5], min(.3, 8 * abs(profile.scale)))
            q[:count // 2, 5] = 0
            q[:count // 2, 6] = 16
            sets.append((q, ids, *native.evaluate(profile, q, ids, slot, 3, .5)))
        q, ids, o, s, x = sets[0]
        valid = (s[:, 10] > 0) & (o[:, 2] >= 0)
        progress(84, "Physical visibility: fitting")
        w = level_three.fit(x[valid], o[valid, 2], preset["vis_steps"], 7112031)
        co, cs, _ = native.evaluate(profile, *sets[1][:2], slot, 3, .5, w)
        cutoff = threshold(co, cs)
        ho, hs, _ = native.evaluate(profile, *sets[2][:2], slot, 3, cutoff, w)
        report["physical_visibility"] = {"threshold": float(cutoff), "held_out": classification(ho, hs)}
    finally:
        profile.scale = scale
    physical = level_three.shader({short: w}).replace("Visibility", "PhysicalVisibility")
    (networks / "PhysicalVisibility.azsli").write_text(
        physical + f"static const float Visibility{short}Threshold = {cutoff:.9e}f;\n")


def train_model_zero(args, work, networks, report):
    progress(10, "Model 0: tracing rays and fitting the seed and hit networks (several minutes)")
    out = work / "model-zero"
    command = [sys.executable, str(HERE / "train_model_zero.py"), str(work / "manifest.json"),
               str(Path(args.tools) / "silpom_welded_teacher.dll"), str(out), "--profile", str(args.profile_id),
               "--scale", str(args.scale), "--reference", str(args.reference), "--address", args.address]
    if args.quick:
        command += ["--training-rays", "40000", "--held-out-rays", "8000", "--seed-steps", "1500", "--hit-steps", "1200"]
    subprocess.run(command, check=True)
    for header in ("NeuralTowerWeights.azsli", "TowerNoMarchWeights.azsli", "NeuralTowerCacheLayout.azsli"):
        if (out / header).exists():
            shutil.copy(out / header, networks / header)
    report["model_zero"] = json.loads((out / "training.json").read_text())
    return out / "NeuralTowerCache.dds" if (out / "NeuralTowerCache.dds").exists() else None


def main():
    cli = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    cli.add_argument("--surface", type=Path, required=True)
    cli.add_argument("--profile-id", type=int, required=True)
    cli.add_argument("--height", type=Path, required=True)
    cli.add_argument("--scale", type=float, required=True)
    cli.add_argument("--reference", type=float, required=True)
    cli.add_argument("--address", choices=("repeat", "clamp"), required=True)
    cli.add_argument("--role", choices=silpom_profile.ROLES, required=True)
    cli.add_argument("--networks", choices=("model0", "hit", "full"), required=True)
    cli.add_argument("--tools", type=Path, required=True, help="Folder with the standalone tool libraries")
    cli.add_argument("--profile", type=Path, required=True, help="The .silpomprofile to write")
    cli.add_argument("--work", type=Path)
    cli.add_argument("--quick", action="store_true", help="Fewer rays and steps, for trying a surface")
    cli.add_argument("--budget", default="6,4,1", help="Solver budget the mesh renders with: "
                     "validation iterations, retry cap, tolerance multiplier")
    cli.add_argument("--rounds", type=int, help="Training rounds, each reweighting the solver's failures "
                     "(default 3, Quick 2)")
    args = cli.parse_args()
    iterations, retries, tolerance = args.budget.split(",")
    args.budget = (int(iterations), int(retries), float(tolerance))
    args.rounds = args.rounds or (2 if args.quick else 3)
    slot = SLOTS.get((args.networks, args.role))
    if slot is None:
        raise SystemExit(f"No {args.networks} network set exists for the {args.role} role")
    for library in ("silpom_welded_teacher.dll", "silpom_level_one.dll", "silpom_visibility.dll"):
        if not (args.tools / library).exists():
            raise SystemExit(f"{library} is not in {args.tools}; build the SilPOM gem standalone (Docs/SurfaceProfiles.md)")
    stem = args.profile.name[:-len(".silpomprofile")]
    args.work = (args.work or args.profile.parent / (stem + ".training")).resolve()
    networks = args.work / "networks"
    networks.mkdir(parents=True, exist_ok=True)
    for old in networks.glob("*.azsli"):
        old.unlink()
    preset = PRESETS["quick" if args.quick else "full"]
    started = time.time()
    progress(2, "Loading the surface and blending face directions")
    profile, dll, faces = surface_profile(args, args.work, args.tools / "silpom_welded_teacher.dll")
    report = {"surface": str(args.surface), "profile_id": args.profile_id, "height": str(args.height),
              "calibration": {"scale": args.scale, "reference": args.reference, "address": args.address},
              "role": args.role, "networks": args.networks, "slot_model": slot, "faces": faces,
              "height_size": profile.atlas.tolist(), "preset": "quick" if args.quick else "full"}
    cache = None
    if args.networks == "model0":
        from train_welded_tower import Profile  # noqa: F401  (load order for train_model_zero)
        _, details = profile.dataset(dll.TraceRays, 20000, 52001, retain_rays=True)
        write_visuals(args.work / "visuals", profile, profile.height, details, faces, profile.surface)
        cache = train_model_zero(args, args.work, networks, report)
    else:
        train_hit(args, preset, profile, dll, slot, networks, report)
        if args.networks == "full":
            train_level_two(args, preset, profile, slot, networks, report)
            train_visibility(args, preset, profile, slot, networks, report)
    report["seconds"] = round(time.time() - started, 1)
    (args.work / "report.json").write_text(json.dumps(report, indent=2) + "\n")

    progress(95, "Writing the profile and its weights")
    folder = args.profile.parent
    entry = {"Version": 1, "Height": os.path.relpath(args.height, folder).replace("\\", "/"),
             "Width": int(profile.atlas[0]), "HeightPixels": int(profile.atlas[1]), "Scale": args.scale,
             "Reference": args.reference, "AddressMode": 1 if args.address == "clamp" else 0,
             "Role": args.role, "ShaderModel": slot,
             "Networks": os.path.relpath(networks, folder).replace("\\", "/")}
    if cache is not None:
        target = folder / (stem + ".seedcache.dds")
        shutil.copy(cache, target)
        entry["Cache"] = target.name
    args.profile.write_text(json.dumps(entry, indent=2) + "\n")
    weights = silpom_profile.weights_path(args.profile, entry)
    if weights.exists():
        weights.unlink()
    if silpom_profile.main(["weights", str(args.profile)]) != 0:
        raise SystemExit("Could not write the profile's weights")
    progress(100, f"Trained in {report['seconds'] / 60:.1f} minutes")
    print("REPORT", args.work / "report.json", flush=True)


if __name__ == "__main__":
    main()

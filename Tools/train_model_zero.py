"""Train SilPOM model 0 from a project's height atlas.

Model 0 has two trained parts, both defined by the shaders:

1. The cached neural depth seed (Assets/Shaders/SilPOM/NeuralTower.azsli).
   * Uncached variant: 4 height probes along the seed ray plus 2 clamped
     slopes -> 8 ReLU units -> a residual on the mean of the middle probes.
     Output: the normalized height of the first hit along the seed ray.
   * Cached variant: a 3072 x 1536 RGBA16F texture. The atlas is read as
     13 x 12 tiles of 512 texels; identical tiles share one of at most 8
     "experts". Each expert stores, for a 7 x 7 grid of ray travel angles,
     the first-hit height at 128 x 128 tile-local entry positions. The tile
     to expert map is TowerNeuralTileRows (NeuralTowerCacheLayout.azsli).
2. The no-march hit model (TowerNoMarch* in BlendedFace.azsli): the same
   11 -> 16 -> 2 squared-ReLU descriptor network as models 1-7, trained for
   model 0's calibration with train_welded_tower.py's rays and fitter.

Labels come from SilPOM's CPU reference (the Tools/welded_tower_teacher.cpp
library: exact heightfield intersection with a 65536-cell march) for the
networks, and from an exact bilinear first-hit search for the cache.

Model 0's calibration defaults to what Source/Mesh.cpp accepts for it:
scale 0.02 m, reference 0.05, clamped addressing.

Outputs, in the output folder:
  model-zero.json                 tensors, cache layout and provenance
  training.json                   held-out accuracy of every part
  NeuralTowerWeights.azsli        uncached seed network (drop-in)
  NeuralTowerCacheLayout.azsli    tile -> expert map (drop-in)
  TowerNoMarchWeights.azsli       hit model; replaces the TowerNoMarch block
                                  in BlendedFace.azsli
  NeuralTowerCache.dds            seed cache, RGBA16F, 1 mip
  NeuralTowerCache.dds.assetinfo  Asset Processor settings for the cache

The .azsli outputs keep the current shaders' symbol names so they can be
checked against the existing paths. They are an interim delivery format until
trained weights become model assets.
"""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import time

os.environ.setdefault("OPENBLAS_NUM_THREADS", "2")
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from train_welded_tower import Profile, fit, score  # noqa: E402

TILE = 512                  # atlas texels per cache tile
TILES = (13, 12)            # cache tile grid (x, y)
EXPERTS = 8                 # 3-bit expert index
ANGLE_STEP, ANGLE_LIMIT = 6.0, 18.0   # texels of travel per angle cell; |delta| limit
ANGLES = 7                  # angle samples per axis (6 cells)
CACHE_RES = 128             # cache texels per tile axis
CACHE_SIZE = (3072, 1536)   # 24 x 12 blocks of 128 texels
TILE_MARGIN = (12.0, 500.0) # tile-local range the shader accepts
SEED_HIDDEN = 8


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load_surface(manifest_path, teacher):
    """Load the manifest's sidecar faces and blend their directions."""
    manifest = json.loads(manifest_path.read_bytes())
    mesh_file = manifest_path.parent / manifest["sidecar"]
    mesh = json.loads(mesh_file.read_bytes())
    faces = sorted(mesh["faces"], key=lambda f: f["stable_id"])
    vertices = np.ascontiguousarray([f["vertices"] for f in faces], dtype=np.uint32)
    xyz = np.ascontiguousarray([[mesh["vertices"][str(v)] for v in face] for face in vertices], dtype=np.float32)
    uv = np.asarray([f["uv"] for f in faces])
    tagged = np.ascontiguousarray([f["region"] != 0 for f in faces], dtype=np.uint32)
    directions = np.empty_like(xyz)
    dll = ctypes.CDLL(str(teacher.resolve()))
    fp = np.ctypeslib.ndpointer(dtype=np.float32, flags="C_CONTIGUOUS")
    up = np.ctypeslib.ndpointer(dtype=np.uint32, flags="C_CONTIGUOUS")
    u, f = ctypes.c_uint, ctypes.c_float
    dll.BlendDirections.argtypes = [fp, up, up, u, fp]
    dll.BlendDirections.restype = ctypes.c_bool
    if not dll.BlendDirections(xyz, vertices, tagged, len(faces), directions):
        raise RuntimeError("The teacher could not blend the sidecar's face directions")
    dll.TraceRays.argtypes = [fp, u, u, f, f, u, fp, u, fp, up, u, fp]
    dll.TraceRays.restype = None
    return manifest, mesh_file, faces, xyz, uv, directions, dll


# ---------------------------------------------------------------------------
# Cached-seed layout: identical atlas tiles share an expert.

def expert_layout(height):
    """Map each 512-texel tile to an expert, or explain why the cache cannot apply."""
    rows, cols = height.shape
    if (cols, rows) != (TILES[0] * TILE, TILES[1] * TILE):
        return None, f"atlas is {cols} x {rows}; the cache format needs {TILES[0] * TILE} x {TILES[1] * TILE}"
    groups, layout, origin = {}, np.zeros((TILES[1], TILES[0]), np.uint32), []
    for y in range(TILES[1]):
        for x in range(TILES[0]):
            key = hashlib.sha1(height[y*TILE:(y+1)*TILE, x*TILE:(x+1)*TILE].tobytes()).digest()
            if key not in groups:
                groups[key] = len(groups)
                origin.append((x, y))
            layout[y, x] = groups[key]
    if len(groups) > EXPERTS:
        return None, f"atlas has {len(groups)} distinct 512-texel tiles; the cache format holds {EXPERTS}"
    return (layout, origin), None


def layout_rows(layout):
    """Pack the tile map as TowerNeuralTileRows: 3 bits per tile, tiles 0-9 in .x, 10-12 in .y."""
    packed = []
    for row in layout:
        x = sum(int(e) << (3 * i) for i, e in enumerate(row[:10]))
        y = sum(int(e) << (3 * i) for i, e in enumerate(row[10:]))
        packed.append((x, y))
    return packed


def bilinear(height, px, py):
    """Sample a normalized atlas at texel-space positions, clamped (matches Profile.sample)."""
    rows, cols = height.shape
    qx = np.clip(px - .5, 0, cols - 1)
    qy = np.clip(py - .5, 0, rows - 1)
    x0, y0 = np.floor(qx).astype(np.int32), np.floor(qy).astype(np.int32)
    x1, y1 = np.minimum(x0 + 1, cols - 1), np.minimum(y0 + 1, rows - 1)
    fx, fy = qx - x0, qy - y0
    return ((height[y0, x0] * (1 - fx) + height[y0, x1] * fx) * (1 - fy)
            + (height[y1, x0] * (1 - fx) + height[y1, x1] * fx) * fy)


def first_hit(height, ex, ey, dx, dy, steps=192, refine=18):
    """Normalized height of the first hit of rays entering the slab top at (ex, ey).

    Texel position at normalized height h is entry - delta * (1 - h). The ray
    hits where the bilinear height reaches h. March down, then bisect.
    """
    h_prev = np.ones_like(ex)
    hit = np.zeros_like(ex)
    found = np.zeros(ex.shape, bool)
    for i in range(1, steps + 1):
        h = 1.0 - i / steps
        inside = bilinear(height, ex - dx * (1 - h), ey - dy * (1 - h)) >= h
        new = inside & ~found
        if new.any():
            lo, hi = np.full(new.sum(), h), h_prev[new] if np.ndim(h_prev) else np.full(new.sum(), h_prev)
            sx, sy, ddx, ddy = ex[new], ey[new], dx[new], dy[new]
            for _ in range(refine):
                mid = .5 * (lo + hi)
                below = bilinear(height, sx - ddx * (1 - mid), sy - ddy * (1 - mid)) >= mid
                lo = np.where(below, mid, lo)
                hi = np.where(below, hi, mid)
            hit[new] = .5 * (lo + hi)
            found |= new
        if found.all():
            break
        h_prev = np.full_like(ex, h)
    return hit


def bake_cache(height, layout, origin):
    """Fill the 3072 x 1536 RGBA16F cache with exact first-hit heights."""
    cache = np.zeros((CACHE_SIZE[1], CACHE_SIZE[0], 4), np.float32)
    k = np.arange(CACHE_RES) * (TILE / (CACHE_RES - 1))          # tile-local entry positions
    lx, ly = np.meshgrid(k, k)
    corners = np.arange(ANGLES) * ANGLE_STEP - ANGLE_LIMIT
    for expert, (tx, ty) in enumerate(origin):
        ex = (tx * TILE + lx).ravel().astype(np.float64)
        ey = (ty * TILE + ly).ravel().astype(np.float64)
        values = np.zeros((ANGLES, ANGLES, CACHE_RES, CACHE_RES), np.float32)
        for cy, delta_y in enumerate(corners):
            for cx, delta_x in enumerate(corners):
                hits = first_hit(height, ex, ey, np.full_like(ex, delta_x), np.full_like(ey, delta_y))
                values[cy, cx] = hits.reshape(CACHE_RES, CACHE_RES)
        for cy in range(ANGLES - 1):
            for cx in range(ANGLES - 1):
                block = expert * 36 + cy * 6 + cx
                bx, by = block % 24, block // 24
                cache[by*CACHE_RES:(by+1)*CACHE_RES, bx*CACHE_RES:(bx+1)*CACHE_RES] = np.stack(
                    (values[cy, cx], values[cy, cx + 1], values[cy + 1, cx], values[cy + 1, cx + 1]), axis=-1)
        print(f"cache: expert {expert} baked", flush=True)
    return cache


def read_cache(cache, layout, entry, delta):
    """CPU replay of SilPomReadTowerNeuralCache. Returns (valid, prediction)."""
    tile = np.floor(entry / TILE)
    valid = (tile >= 0).all(1) & (tile < np.array(TILES)).all(1) & (np.abs(delta) <= ANGLE_LIMIT).all(1)
    local = entry - tile * TILE
    exit_ = local - delta
    valid &= (np.minimum(local, exit_) >= TILE_MARGIN[0]).all(1) & (np.maximum(local, exit_) <= TILE_MARGIN[1]).all(1)
    tile = np.clip(tile, 0, np.array(TILES) - 1).astype(int)
    expert = layout[tile[:, 1], tile[:, 0]]
    angle = (delta + ANGLE_LIMIT) / ANGLE_STEP
    cell = np.minimum(np.floor(angle), 5).astype(int)
    blend = angle - cell
    block = expert * 36 + cell[:, 1] * 6 + cell[:, 0]
    texel = np.column_stack((block % 24, block // 24)) * CACHE_RES + local * ((CACHE_RES - 1) / TILE)
    # Bilinear texture filtering at texel centers.
    q = texel
    i0 = np.floor(q).astype(int)
    f = q - i0
    i1 = i0 + 1
    i0 = np.clip(i0, 0, np.array(CACHE_SIZE) - 1)
    i1 = np.clip(i1, 0, np.array(CACHE_SIZE) - 1)
    c = cache.astype(np.float16).astype(np.float32)   # stored precision
    s = ((c[i0[:, 1], i0[:, 0]] * (1 - f[:, :1]) + c[i0[:, 1], i1[:, 0]] * f[:, :1]) * (1 - f[:, 1:])
         + (c[i1[:, 1], i0[:, 0]] * (1 - f[:, :1]) + c[i1[:, 1], i1[:, 0]] * f[:, :1]) * f[:, 1:])
    bx, by = blend[:, 0], blend[:, 1]
    prediction = (s[:, 0] * (1 - bx) + s[:, 1] * bx) * (1 - by) + (s[:, 2] * (1 - bx) + s[:, 3] * bx) * by
    return valid, prediction


def score_cache(height, layout, cache, count, seed, scale):
    rng = np.random.default_rng(seed)
    entry = rng.uniform(0, np.array(TILES) * TILE, (count, 2))
    delta = rng.uniform(-ANGLE_LIMIT, ANGLE_LIMIT, (count, 2))
    valid, prediction = read_cache(cache, layout, entry, delta)
    entry, delta, prediction = entry[valid], delta[valid], prediction[valid]
    exact = first_hit(height, entry[:, 0], entry[:, 1], delta[:, 0], delta[:, 1])
    error_mm = np.abs(prediction - exact) * scale * 1000
    return {"rays": int(valid.sum()), "rays_outside_cache_domain": int((~valid).sum()),
            "vertical_error_p50_mm": float(np.median(error_mm)),
            "vertical_error_p95_mm": float(np.quantile(error_mm, .95)),
            "within_4mm": float((error_mm <= 4).mean())}


def write_dds_rgba16f(path, cache):
    height, width, _ = cache.shape
    header = struct.pack("<7I11I8I5I", 124, 0x100F, height, width, width * 8, 0, 1,
                         *([0] * 11), 32, 4, 0x30315844, 0, 0, 0, 0, 0,
                         0x1000, 0, 0, 0, 0)
    dx10 = struct.pack("<5I", 10, 3, 0, 1, 0)   # DXGI_FORMAT_R16G16B16A16_FLOAT, TEXTURE2D
    path.write_bytes(b"DDS " + header + dx10 + cache.astype("<f2").tobytes())


ASSETINFO = """<ObjectStream version="3">
  <Class name="TextureSettings" version="2" type="{980132FF-C450-425D-8AE0-BD96A8486177}">
    <Class name="Name" field="Preset" value="LUT_RGBA16F" type="{3D2B920C-9EFD-40D5-AAE0-DF131C3D4931}"/>
    <Class name="unsigned int" field="SizeReduceLevel" value="0" type="{43DA906B-7DEF-4CA8-9790-854106D3F983}"/>
    <Class name="bool" field="EngineReduce" value="false" type="{A0CA880C-AFE4-43CB-926C-59AC48496112}"/>
    <Class name="bool" field="EnableMipmap" value="false" type="{A0CA880C-AFE4-43CB-926C-59AC48496112}"/>
  </Class>
</ObjectStream>
"""


# ---------------------------------------------------------------------------
# Uncached seed network.

def seed_samples(profile, details):
    """Rebuild SilPomTryTowerNeural's seed rays, probes and labels for traced rays."""
    rays, ids, teacher = details["rays"].astype(np.float64), details["ids"], details["teacher"]
    camera, ray = rays[:, :3], rays[:, 3:]
    directions = profile.directions[ids]
    normal = profile.normal[ids]
    incidence = np.minimum(-np.sum(normal * ray, axis=1),
                           np.min(-np.einsum("nij,nj->ni", directions, ray), axis=1))
    origin, line = profile.project(ids, directions.mean(axis=1), camera - profile.xyz[ids, 0], ray)
    keep = (incidence >= .30) & (line[:, 2] < -.01) & (teacher[:, 0] == 1)
    origin, line, ids, incidence, t_hit = origin[keep], line[keep], ids[keep], incidence[keep], teacher[keep, 1]
    uv0 = profile.uv[ids, 0]
    levels = (np.arange(4) + .5) * .25
    t = (profile.scale * (levels[None] - profile.reference) - origin[:, 2:3]) / line[:, 2:3]
    coords = origin[:, None, :2] + line[:, None, :2] * t[..., None] + uv0[:, None]
    heights = profile.sample(coords)
    slopes = np.clip(line[:, :2] * (profile.scale / (16 * line[:, 2:3])) * profile.atlas, -2, 2)
    target = (origin[:, 2] + line[:, 2] * t_hit) / profile.scale + profile.reference
    # Seed acceptance tolerates |t error| * incidence <= 4 mm; express the
    # loss in those units: dt = dh * scale / |dz|.
    weight = profile.scale * incidence / np.abs(line[:, 2])
    return (np.column_stack((2 * heights - 1, slopes)).astype(np.float32), heights.astype(np.float32),
            target.astype(np.float32), weight.astype(np.float32))


def seed_forward(w, features, heights):
    pre = features @ w["w"].T + w["b"]
    return (heights[:, 1] + heights[:, 2]) * .5 + w["out_bias"] + np.maximum(pre, 0) @ w["out"], pre


def fit_seed(data, seed, steps=6000, batch=2048):
    features, heights, target, weight = data
    rng = np.random.default_rng(seed)
    w = {"w": rng.normal(0, .3, (SEED_HIDDEN, 6)).astype(np.float32),
         "b": rng.normal(0, .1, SEED_HIDDEN).astype(np.float32),
         "out": rng.normal(0, .05, SEED_HIDDEN).astype(np.float32),
         "out_bias": np.float32(0)}
    m = {k: np.zeros_like(v) for k, v in w.items()}
    v2 = {k: np.zeros_like(v) for k, v in w.items()}
    for step in range(1, steps + 1):
        ids = rng.integers(len(target), size=batch)
        x, h, y, s = features[ids], heights[ids], target[ids], weight[ids]
        prediction, pre = seed_forward(w, x, h)
        # Huber loss on the 4 mm acceptance scale, in metres.
        e = (prediction - y) * s
        g = np.clip(e, -.004, .004) * s / batch
        active = np.maximum(pre, 0)
        grad_hidden = (g[:, None] * w["out"]) * (pre > 0)
        grads = {"w": grad_hidden.T @ x, "b": grad_hidden.sum(0), "out": active.T @ g, "out_bias": g.sum()}
        lr = .01 if step < steps * .7 else .002
        for k in w:
            m[k] = .9 * m[k] + .1 * grads[k]
            v2[k] = .999 * v2[k] + .001 * grads[k] ** 2
            w[k] = (w[k] - lr * (m[k] / (1 - .9 ** step)) / (np.sqrt(v2[k] / (1 - .999 ** step)) + 1e-12)).astype(np.float32)
    return w


def score_seed(data, w):
    features, heights, target, weight = data
    prediction, _ = seed_forward(w, features, heights)
    error_mm = np.abs(np.clip(prediction, 0, 1) - target) * weight * 1000
    return {"rays": int(len(target)), "seed_error_p50_mm": float(np.median(error_mm)),
            "seed_error_p95_mm": float(np.quantile(error_mm, .95)),
            "within_4mm": float((error_mm <= 4).mean())}


# ---------------------------------------------------------------------------
# Shader text.

def fmt(values):
    return ", ".join(f"{float(v):.9e}f" for v in values)


def seed_header(w, provenance):
    lines = ["// SPDX-License-Identifier: MIT", f"// Generated by Tools/train_model_zero.py. {provenance}", "#pragma once",
             "static const float4 TowerNeuralHeightWeights[8] = {",
             *[f"    float4({fmt(row[:4])})," for row in w["w"]], "};",
             "static const float2 TowerNeuralSlopeWeights[8] = {",
             *[f"    float2({fmt(row[4:])})," for row in w["w"]], "};",
             f"static const float TowerNeuralBias[8] = {{{fmt(w['b'])}}};",
             f"static const float TowerNeuralOutput[8] = {{{fmt(w['out'])}}};",
             f"static const float TowerNeuralOutputBias = {float(w['out_bias']):.9e}f;"]
    return "\n".join(lines) + "\n"


def layout_header(rows, provenance):
    lines = ["// SPDX-License-Identifier: MIT", f"// Generated by Tools/train_model_zero.py. {provenance}",
             "// Row zero is the north edge (first image row) of the height atlas.", "#pragma once",
             f"static const uint2 TowerNeuralTileRows[{len(rows)}] = {{",
             *[f"    uint2(0x{x:08x}u, 0x{y:08x}u)," for x, y in rows], "};"]
    return "\n".join(lines) + "\n"


def hit_header(w, provenance):
    lines = [f"// Generated by Tools/train_model_zero.py. {provenance}",
             "// Replaces the TowerNoMarch block in BlendedFace.azsli.",
             "static const float TowerNoMarchW0[16][11] = {",
             *["    {" + fmt(row) + "}," for row in w[0].T], "};",
             f"static const float TowerNoMarchB0[16] = {{{fmt(w[1])}}};",
             "static const float2 TowerNoMarchW1[16] = {",
             *["    float2(" + fmt(row) + ")," for row in w[2]], "};",
             f"static const float2 TowerNoMarchB1 = float2({fmt(w[3])});"]
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------

def main():
    cli = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    cli.add_argument("manifest", type=Path, help="Training manifest: sidecar and height profiles")
    cli.add_argument("teacher", type=Path, help="Library built from Tools/welded_tower_teacher.cpp")
    cli.add_argument("output", type=Path)
    cli.add_argument("--profile", default="1", help="Manifest profile whose faces and height atlas model 0 uses")
    cli.add_argument("--scale", type=float, default=.02, help="Model 0 relief scale in metres")
    cli.add_argument("--reference", type=float, default=.05)
    cli.add_argument("--address", choices=("repeat", "clamp"), default="clamp")
    cli.add_argument("--training-rays", type=int, default=230000)
    cli.add_argument("--held-out-rays", type=int, default=48000)
    cli.add_argument("--seed-steps", type=int, default=6000)
    cli.add_argument("--hit-steps", type=int, default=3300)
    cli.add_argument("--skip-cache", action="store_true", help="Do not bake the seed cache")
    cli.add_argument("--compare", type=Path, help="SilPOM checkout: also score its current model 0 weights")
    args = cli.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    started = time.time()

    manifest, mesh_file, faces, xyz, uv, directions, dll = load_surface(args.manifest, args.teacher)
    config = dict(manifest["profiles"][args.profile])
    height_file = args.manifest.parent / config["height"]
    config.update(scale_metres=args.scale, reference=args.reference, address=args.address,
                  tiling=config.get("tiling", [1, 1]), offset=config.get("offset", [0, 0]))
    keep = np.asarray([face["region"] and face["profile"] == int(args.profile) for face in faces], dtype=bool)
    if not keep.any():
        raise SystemExit(f"No tagged faces use profile {args.profile} in {mesh_file}")
    profile = Profile(config, height_file, xyz[keep], uv[keep], directions[keep])
    provenance = (f"Atlas {height_file.name} sha256 {sha256(height_file)[:16]}, scale {args.scale} m, "
                  f"reference {args.reference}, {args.address}.")
    report = {"tool": "Tools/train_model_zero.py", "calibration": config,
              "height_sha256": sha256(height_file), "surface_sha256": sha256(mesh_file),
              "manifest_sha256": sha256(args.manifest), "faces": int(keep.sum()),
              "height_size": profile.atlas.tolist(), "seeds": {"train": 52001, "test": 52002, "hit": 52003, "seed": 52004, "cache": 52005}}

    print(f"Tracing {args.training_rays} training and {args.held_out_rays} held-out rays", flush=True)
    train, train_details = profile.dataset(dll.TraceRays, args.training_rays, 52001, retain_rays=True)
    test, test_details = profile.dataset(dll.TraceRays, args.held_out_rays, 52002, retain_rays=True)

    print("Fitting the no-march hit model", flush=True)
    hit = fit(train, 52003, steps=args.hit_steps,
              checkpoint_steps=tuple(s for s in (800, 1600, 2500, args.hit_steps) if s <= args.hit_steps))
    report["hit_model"] = {"training_rays": len(train[0]), "held_out": score(test, hit)}

    print("Fitting the uncached seed network", flush=True)
    seed_train, seed_test = seed_samples(profile, train_details), seed_samples(profile, test_details)
    seed = fit_seed(seed_train, 52004, steps=args.seed_steps)
    report["seed_network"] = {"training_rays": len(seed_train[2]), "held_out": score_seed(seed_test, seed)}

    tensors = {"hit_model": {"w0": hit[0].T.tolist(), "b0": hit[1].tolist(), "w1": hit[2].tolist(), "b1": hit[3].tolist()},
               "seed_network": {"height_weights": seed["w"][:, :4].tolist(), "slope_weights": seed["w"][:, 4:].tolist(),
                                "bias": seed["b"].tolist(), "output": seed["out"].tolist(),
                                "output_bias": float(seed["out_bias"])}}

    if args.compare:
        report["current_weights"] = compare_current(args.compare, test, seed_test)

    (args.output / "NeuralTowerWeights.azsli").write_text(seed_header(seed, provenance), encoding="utf-8")
    (args.output / "TowerNoMarchWeights.azsli").write_text(hit_header(hit, provenance), encoding="utf-8")

    layout, reason = expert_layout(np.asarray(profile.height))
    if args.skip_cache:
        report["cache"] = {"skipped": "--skip-cache"}
    elif layout is None:
        report["cache"] = {"skipped": reason}
        print("Cache not baked:", reason, flush=True)
    else:
        tile_map, origin = layout
        print(f"Baking the seed cache for {len(origin)} experts", flush=True)
        cache = bake_cache(profile.height, tile_map, origin)
        write_dds_rgba16f(args.output / "NeuralTowerCache.dds", cache)
        (args.output / "NeuralTowerCache.dds.assetinfo").write_text(ASSETINFO, encoding="utf-8")
        rows = layout_rows(tile_map)
        (args.output / "NeuralTowerCacheLayout.azsli").write_text(layout_header(rows, provenance), encoding="utf-8")
        report["cache"] = {"experts": len(origin), "expert_tile_origins": origin,
                           "dds_sha256": sha256(args.output / "NeuralTowerCache.dds"),
                           "held_out": score_cache(profile.height, tile_map, cache, 200000, 52005, args.scale)}
        tensors["cache_layout"] = {"tile_rows": [list(r) for r in rows], "tile_map": tile_map.tolist()}

    report["seconds"] = round(time.time() - started, 1)
    (args.output / "model-zero.json").write_text(json.dumps({"provenance": report, "tensors": tensors}) + "\n", encoding="utf-8")
    (args.output / "training.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: report[k] for k in ("hit_model", "seed_network", "cache")}, indent=2))


def compare_current(checkout, hit_test, seed_test):
    """Score the weights currently compiled into a SilPOM checkout on the same held-out rays."""
    import re
    shaders = checkout / "Assets/Shaders/SilPOM"
    numbers = lambda text: np.array([float(v) for v in re.findall(r"[-+]?\d+\.\d+e[-+]\d+", text)], np.float32)
    blended = (shaders / "BlendedFace.azsli").read_text(encoding="utf-8")
    block = re.search(r"static const float TowerNoMarchW0.*?static const float2 TowerNoMarchB1[^;]*;", blended, re.S).group(0)
    values = numbers(block)
    w0 = values[:176].reshape(16, 11).T
    hit = [w0, values[176:192], values[192:224].reshape(16, 2), values[224:226]]
    seed_values = numbers((shaders / "NeuralTowerWeights.azsli").read_text(encoding="utf-8"))
    hw, sw = seed_values[:32].reshape(8, 4), seed_values[32:48].reshape(8, 2)
    seed = {"w": np.hstack((hw, sw)), "b": seed_values[48:56], "out": seed_values[56:64], "out_bias": seed_values[64]}
    return {"hit_model": score(hit_test, hit), "seed_network": score_seed(seed_test, seed)}


if __name__ == "__main__":
    main()

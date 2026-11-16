# Experimental consolidated paint color/normal cache

**Disabled and excluded from the default engine bundle.** The tower benchmark
rejected this world-space lattice implementation: it added shader cost, accepted
no painted samples in the saved tower views, and introduced excessive normal
resampling error in the small patch where it did accept paint. See
[the measured results](DecalPaintBakeBenchmark.md). Retained as an experiment,
not presented as a completed production optimization. A receiver-surface UV
atlas is the next design to evaluate.

Consolidation still stores resolved dabs. After 0.75 seconds without a document
change, the renderer can bake a fixed prefix of each layer in each spatial group
into derived GPU images. Live dabs keep their ordered projection evaluation.
Neither this cache nor the painter writes SiLPOM's heightmap, relief bounds,
geometry, learned model or source material textures. Height masks only select
which existing relief heights receive paint.

## Representation and fallback

A spatial lattice is flattened into a 2D slice atlas with one-texel gutters.
Each prefix has four RGBA16F images: color scale, color bias, a representative
world normal, and receiver-normal validation data. Scale and bias preserve
`color_out = scale * layer_start_color + bias`; the source color need not be
known when the bake runs. Alpha, Multiply, Screen and layer erases preserve
this form. Add and Overlay depend nonlinearly on the incoming color, so the
eligible prefix ends before them. Prefixes shorter than 32 dabs are left alone.

The cache keeps original dabs, order and shared material texture arrays. It
can skip a fixed prefix only when its certificates permit it. Empty cells use
conservative projection bounds expanded by a full lattice neighbourhood,
so a narrow stamp between samples cannot be silently discarded. Occupied cells
require a pixel footprint near the cache spacing, eight receiver-normal samples
within five degrees of the actual receiver normal, a conservative attenuation
color-error bound, and either unchanged normals or a converged normal chain.
Convergence uses a 45-degree normal cone only when every baked target and the runtime incoming
normal satisfy it; otherwise the full unit-sphere bound remains in effect.
Uncertain normals, close views, strong minification and unsupported operations
fall back to the original projection stream. These guards do not make filtered
texture resampling lossless; image comparisons must measure that approximation.

Low, middle, high and unknown receiver-height selections have separate fields.
If all masks select every height, only one field is allocated. The SiLPOM height
value is read for selection; its height texture is never altered. Erases retain
the layer-start color and normal semantics, including erases in the live tail.

## Lifetime and controls

Images are a disposable runtime cache, not newly saved assets. Document,
transform, material or source image changes invalidate affected entries. The
original paint document remains sufficient to rebuild everything. Shader
reloads restart in-progress jobs. A bounded row workload is submitted over
multiple frames, one prefix per entity at a time. A shader-read finalize pass
establishes resource transitions before publication; both bake passes are
removed afterwards, so no bake draw remains in steady-state timings.

- `r_silpomPaintBake`: enable/disable the cache (default **false**). Requires
  installing the explicit experimental shader overlay first.
- `r_silpomPaintBakeDensity`: target lattice samples per world metre (128).
- `r_silpomPaintBakeMiB`: image-memory budget per paint entity (128 MiB), divided
  across groups/layers. Each dimension is capped at 512 samples. Four RGBA16F
  images cost 32 bytes per atlas texel, including gutters and height variants.
- `r_silpomPaintBakeDebug`: green means an empty cached prefix was skipped;
  blue means cached color/normals were used; red means projection fallback. Value 2 encodes rejection reasons in RGB bits
  (1 footprint, 2 attenuation error, 3 receiver normal, 4 normal dependence,
  5 bounds, 6 HDR/nonfinite input, 7 incoming normal outside its certified cone).
- `DecalPaintRequestBus.GetRenderStatsJson`: ready/pending/failed prefix counts,
  allocated image bytes and total wall time from request to final publication,
  in addition to group, buffer and rebuild statistics.

Runtime allocation is bounded per entry; tiny budgets can be exceeded by the
minimum 2x2x2 lattice and per-job status image. Retained source projections and
the shared raw buffer remain allocated for fallback and editing.

## Reproducing the comparison

First install the opt-in engine overlay with `Integration/O3DE/apply.py
--engine ENGINE --project PROJECT --experimental-paint-bake`, build this branch's
native modules, and let Asset Processor rebuild every dependent shader/material.
The ordinary installer selects the stable shader; running it without the flag
restores an unedited experiment by its exact hash. Rebuild assets after restoring.
The profiling build script also selects the stable bundle by default.

Run `Tests/decal_bake_benchmark.py` using Editor's `--runpython` in an isolated
session, with `TG_SILPOM_ESTIMATOR_OUTPUT` set to a fresh evidence directory.
`TG_PAINT_BAKE_MODES=0,1,1,0` performs a matched ABBA comparison. The saved
13,000-dab tower paint, mixed-operation spread fixture and compact overlapping
normal-paint patch are timed separately. Cameras, 1280x720 resolution and 2x
MSAA are fixed. Each timed run captures 180 GPU timestamp frames after warmup;
baking and image captures are excluded from those intervals. The source level
is never saved and is checked for byte equality on exit.

Run `Tests/analyze_decal_bake_benchmark.py EVIDENCE_DIRECTORY` to report GPU spans,
HitForward time, raw hit equality, lit diffuse error, packed-normal angular error,
cache coverage and visible paint contribution. Debug captures and no-paint
controls establish whether timing changes actually include cached paint. GPU
span includes inter-pass waits; CPU frame period includes waits and is not a
measurement of isolated CPU work.

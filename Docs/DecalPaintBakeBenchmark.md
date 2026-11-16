# Color/normal paint bake experiment: 2026-11-15

**Result: rejected for production.** The prototype is disabled in native code.
The default engine bundle retains the fast `f9e3d7c` spatial-composite shader;
the experimental shader requires an explicit installer flag. No SiLPOM height
texture, relief geometry, learned weights or source paint document was changed.

This implementation bakes fixed prefixes into world-space slice atlases. It is
not a receiver-surface UV atlas. Retaining it as an experiment makes the failed
trade-offs and test coverage available without imposing its GPU cost on normal
builds. A surface/UV bake remains future work, not a completed optimization.

## Measurements

AMD Radeon RX 7900 XTX, driver 32.0.32015.7029; Ryzen 7 2700; profile DX12 SM 6.7;
gameplay, 1280x720, 2x MSAA. Each run captures 180 timestamp frames after warmup.
The prototype uses a matched ABBA cache 0/1/1/0 sequence; values below are medians
of the two per-run medians. Baking, screenshots and readbacks are excluded from
steady-state timings. The original spatial build has two separate repeats.

GPU graphics span in milliseconds (includes inter-pass waits):

| Fixture/view | Original spatial renderer | Prototype, cache off | Prototype, cache on | On vs off |
|---|---:|---:|---:|---:|
| saved-inside | 7.100 | 8.152 | 8.168 | +0.2% |
| saved-outside | 6.324 | 6.389 | 6.454 | +1.0% |
| spread-inside | 19.764 | 47.106 | 46.744 | -0.8% |
| spread-outside | 6.794 | 7.362 | 7.380 | +0.2% |
| patch-patch | â€” | 9.307 | 9.904 | +6.4% |

Including the prototype's shader code regressed the uncached path as well.
Turning only its runtime cache flag off does not remove that shader cost. This
is why the experimental shader is excluded from the ordinary integration patch.
The spread fixture has no detectable painted color or normal pixels in these
cameras; use it as a projection/traversal stress case, not a visible-paint quality
control. The patch provides the positive visible-paint control.

## Coverage and quality

Saved tower paint: 12,940 projections, 32 groups, one shared material/texture set.
All 32 prefixes baked, allocating 130,317,664 image payload bytes (124.3 MiB) in
15.825 seconds. Neither saved camera accepted a painted field sample; most
occupied samples failed the footprint test, with smaller receiver-normal/error
rejections. Empty-region skipping did not deliver a useful timing gain.

Dense patch: 2,048 consolidated normal-painted dabs, one group, 4,204,808 image
payload bytes (4.0 MiB), 0.575-second bake. It accepted 71 cached pixels, including
71 pixels visibly changed by original color or normal paint. In those painted
cached pixels, normal error was 6.77 degrees RMS and 15.85 degrees maximum.
Lit diffuse error was 0.00001728 RMS and 0.00007594 maximum in linear units.
The preselected normal checks (RMS <=1 degree, maximum <=5 degrees) failed.
The incoming-normal dependence certificate does not bound errors introduced by
spatial resampling and mismatched filtering footprints.

Captured hit records were byte-identical with cache off/on in all five views.
The source VueLiveLink prefab stayed byte-identical (SHA-256
`a0cd36ade8a6ef7fb9492908556e93ca61c022e2450685ae1eacff8018d51689`).

## Crash and safe defaults

Prototype activation initially hit a null image-pool allocation. The engine's
request header documents a default pool, but its implementation dereferences the
pointer. The prototype now supplies the system pool explicitly for every image.
The corrected isolated Editor completed both validation and the full benchmark
with normal exit codes. The shutdown TickBus fault seen after the initial
exception did not recur in those corrected runs.

`apply.py --experimental-paint-bake` selects the isolated experimental shader;
ordinary application restores its exact unedited predecessor to stable. Both
modes retain hash checks and preserve local edits. Native `r_silpomPaintBake`
defaults to false. Standard installed Editor/runtime DLLs remain the original
spatial build. The restored shader/material assets rebuilt with zero failures.
The standard Editor then loaded VueLiveLink and completed two timing repeats
with a clean exit. Restored GPU spans were:

| Fixture/view | Restored GPU span (ms) | Restored HitForward (ms) |
|---|---:|---:|
| patch-patch | 8.068 | 3.646 |
| saved-inside | 7.015 | 1.609 |
| saved-outside | 6.056 | 1.099 |
| spread-inside | 19.730 | 13.359 |
| spread-outside | 6.664 | 1.510 |

This returns to the original spatial-renderer range, rather than leaving the
experimental shader's disabled-path regression installed. The rebuilt, disabled
prototype module also passed the painter's loading, 5,000-dab retention,
undo/redo, prefab round-trip, long-stroke and rendered-reference smoke checks.
Reference comparison maximum error was 1 RGB level; no Editor assertions were
recorded. The source level hash remained unchanged after every final check.

## Evidence and reproduction

Machine-readable runs, cache coverage, rejection reasons, errors and input hashes
are in [the benchmark JSON](Benchmarks/DecalPaintBake-20261115.json).
Raw local evidence is in `scratch/decal-bake/before`, `after-cone`, and
`qa-pool-fix`. `Tests/decal_bake_benchmark.py` and its analyzer reproduce the
measurements. CPU frame period includes waits and is not isolated CPU work;
these GPU spans are not guaranteed frame rates. Driver/pool allocation overhead
is not included in the image payload byte counts.

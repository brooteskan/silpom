# Receiver-UV paint benchmark, 2026-11-15

The receiver-owned 2D bake completed the full gameplay run after fixing the
fullscreen material lookup and binding the retained pipeline's object SRG.
There were no device removals or skipped resource bindings in the final run.
**The normal bake fails quality acceptance and remains opt-in.**

One existing tower face owns the cache. Each source dab carries receiver entity,
canonical face and UV before consolidation. The shader uses the receiving hit's
face and UV; camera position is absent from ownership. No 3D lattice is allocated
for this path. Neither stamp height nor derived paint is written into SiLPOM's
heightmap. The source document, level and height images are retained.

## Method and performance

RX 7900 XTX / Ryzen 7 2700, 1280×720 gameplay, 2× MSAA. One shared stamp
material, 2048 dabs consolidated by the actual controller and 100 live dabs.
Modes ran in order 0,1,2,2,1,0, with 180 timestamp frames per view and run.
Values below average the two run medians. Captures and diagnostic passes were
disabled during timing. GPU span includes scheduling waits; CPU frame period
is recorded separately and is not an isolated CPU-work measurement.

| View | Native replay (ms) | UV path, replay (ms) | UV atlas (ms) | Atlas vs native |
| --- | ---: | ---: | ---: | ---: |
| Close | 16.091 | 16.608 | 13.631 | -15.3% |
| Ordinary | 10.011 | 10.634 | 9.695 | -3.2% |
| Minified | 8.505 | 9.569 | 9.239 | +8.6% |

The cache helps the close view, has little benefit in the ordinary view, and
regresses the minified view. The disabled UV path itself costs extra shader
work. CPU frame-period data do not establish an ordinary/minified frame-rate
improvement.

The 512² chart uses ten independently rendered mips and 8,588,856 image bytes
(8.19 MiB). Amortized wall time to build it was 6.56 seconds, including frame
scheduling and pass setup; this is not a standalone GPU bake duration.

## Geometry, ownership and actual use

Both real MSAA samples were exported individually through single-sample atlases.
Every native/replay/cache hit record and depth bit matched at all three views.
Prefix replay also matched native shaded color exactly. Other receiving faces
were unchanged. Per-sample UINT4 diagnostics reported these atlas-use counts:

| View | Sample 0 | Sample 1 |
| --- | ---: | ---: |
| Close | 25,489 | 25,582 |
| Ordinary | 3,379 | 3,355 |
| Minified | 1,229 | 1,199 |

The diagnostic separately records no-attempt, replay and atlas use, receiving
face, replaced-prefix count and owner token. Counts are not inferred from RGB.
Removing the fixed prefix proved visible fixed-paint contribution in 51,071
real MSAA samples. Editing the live tail
changed the result without rebuilding the fixed atlas. Invalid-face and empty
prefix cases reported zero cache attempts. Camera changes did not rebuild or
reassign the atlas. Source level and height-image hashes were unchanged within
the final run.

## Color and normal accuracy

Across cached samples, worst-sample linear diffuse RMS was 0.000505 close,
0.000258 ordinary and 0.000190 minified. Worst absolute diffuse error was 0.00465.
Normal error is materially worse:

| View | Worst sample RMS (degrees) | Largest error (degrees) |
| --- | ---: | ---: |
| Close | 2.65 | 32.34 |
| Ordinary | 5.15 | 61.68 |
| Minified | 5.82 | 43.54 |

These exceed the retained 1° RMS / 5° maximum gate. The current normal
coefficient/filter approximation needs further work before enabling the cache
for normal painting. The measurements establish a complete surface-owned bake
and its limits; they do not establish acceptable normal quality or a general
tower performance win.

## Crash correction and validation

The zero-argument material getter used a mesh ObjectSrg to select scene-material
parameters. A fullscreen pass has no implicit mesh draw. The bake now supplies
explicit material type/instance indices, binds the receiver's real ObjectSrg
for the retained pipeline layout, and supplies a defined instance offset.
All three compiled shader dependency manifests are audited. A bounded bake
traversal also prevents malformed node counts from creating an unbounded loop.

The final run uses complete bindings and rejects any skipped draw. Earlier
crashed or skipped-draw runs were not used for this report. The source level
was not saved by the test; its current user-edited baseline was preserved.

Detailed per-sample data, timings and source hashes:
[ReceiverUvPaint-20261115.json](Benchmarks/ReceiverUvPaint-20261115.json).
Design and limits: [ReceiverUvPaint.md](ReceiverUvPaint.md).

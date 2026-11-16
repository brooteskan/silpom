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

## Validation corrections after review

The retained run above predates the strengthened image checks. Its zero-usage
records establish that invalid-face and cleared documents did not use the UV
prefix; they do not establish that all paint disappeared. Its other-face
comparison checked color only, and its replay normal metric had an empty mask.
Those parts of the retained evidence are incomplete.

The capture harness now exports a native no-paint baseline before submitting
the fixture stamps. The analyzer requires that baseline and compares both real
MSAA samples of cleared and orphaned documents against its color, packed
normals, hit records and depth. Replay requires a nonempty replay mask and
bit-exact native color and packed normals. Other-face preservation checks both
color and packed normals. Atlas error remains measured against the retained
0.01 RMS / 0.05 maximum color and 1 degree RMS / 5 degree maximum normal gates;
the report records failed quality gates instead of treating an empty mask as
success.

`Tests/test_receiver_uv_analysis.py` supplies synthetic negative controls for
stale color/normals despite zero usage, changed replay/other-face normals,
empty replay masks and atlas normal-quality failure. These CPU tests do not
replace a new GPU capture. The earlier capture set lacks the no-paint baseline
and must be regenerated before the stronger analyzer can validate it. No
post-fix performance or visual-quality result is implied by the earlier run.

## Post-fix correctness recapture, 2026-11-16

The rebuilt runtime and Editor and their matching shader assets completed a
fresh 2,048-fixed / 100-live-dab run at 512 chart resolution and 2x MSAA.
The strengthened analyzer passed these checks on both real samples:

- Prefix replay matches native color and packed normals bit-for-bit in all
  three views, with nonempty replay masks.
- Other-face color and normals remain bit-exact in replay and atlas modes.
- Orphaned and cleared documents match the pre-fixture no-paint baseline in
  color, packed normals, hit records and depth; neither reports prefix use.
- Live-tail edits retain the fixed atlas. Camera moves retain it as well.
- Fixed paint makes a visible contribution on 51,071 samples. No draw was
  skipped for missing resource bindings; the level and height images are unchanged.

Atlas quality still fails: normal RMS spans 2.60–5.82 degrees and the maximum
reaches 61.66 degrees, exceeding 1 degree RMS / 5 degree maximum. The atlas is
not accepted and remains opt-in. The run also recorded recurring, existing
Falconry `normalFov` script errors; its timings are observations, not a clean
performance acceptance run or a replacement for the historical table above.

Native compilation passed for `SilPOM.Static`, `SilPOM.Editor` and `SilPOM`.
Asset processing completed 815 jobs with zero failed assets and zero errors
(554 warnings). All three compiled bake binding manifests passed audit.
The 16 portable C++ tests, 11 analyzer negative controls, extracted native
shader traversal test and 25 installer scenarios passed; CPU surface tests
also passed AddressSanitizer.

Metrics, source hashes and validation context are retained in
[ReceiverUvPaint-20261116.json](Benchmarks/ReceiverUvPaint-20261116.json).

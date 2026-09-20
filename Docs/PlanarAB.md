# Planar replacement — issue #3

Implementation base: `58eba173b5dd269df4b4846b6ac5c38d11d8cd68`, on the local
`main` checkout as requested. The experiment is preserved on
`codex/preserve-silpom-modifications-20260919` at `fb69046`. No experimental curved
solver, compute hit buffer, or new acceleration structure is used here.
This report accompanies the planar implementation and benchmark changes.
The existing `.gitignore` edit and `Tests/Fixtures` are user-owned and excluded
from the implementation commit.

## Implemented path

- `Source/Patch.cpp`: four vertices, six indices; RT and motion proxy draws off.
  Camera visibility can be toggled independently of the flat shadow caster.
- `Assets/Materials/Types/SilPOM/SilPOM.azsli`: conservative camera rectangle,
  exact bilinear hit UV/normal/depth, miss discard; `SHADOWMAP` instead transforms
  only the original finite rectangle and does not include the height kernel.
  Atom couples custom camera depth with its custom-depth shadow variant, so the
  latter passes hardware-interpolated shadow depth through a trivial pixel stage.
- `Assets/Shaders/SilPOM/Heightfield.azsli`: skips the quadratic root solve when
  the ray's cell interval cannot overlap the four-corner height range. The bounds
  include signed scale and numerical padding; the exact root calculation and
  nearest-hit traversal order are unchanged.
- `Source/Editor/PlanarBenchmark.cpp`: opt-in editor fixture generates a regular
  reference grid, uses TG's existing vertex-displacement material, suppresses its
  own shadow/motion draws, and leaves the same flat patch casting in both modes.
  No fixture is created until requested. The overlay remains enabled in A/B runs.
- `Tests/planar_ab.py`: native-size viewport, render scale 1, fixed camera paths,
  warm-up, alternate A/B run order, raw per-pass GPU timestamps, screenshots,
  linear depth, and separate failure-mask captures. No level is saved.

## Reproduction in TG

Build `SilPOM` and `SilPOM.Editor` in the existing project build, then:

```powershell
python Tools/prepare_planar_ab_fixture.py --project D:/TG/TGProject
python Tools/prepare_planar_ab_level.py --project D:/TG/TGProject --name SilPOMPlanarAB_20260919
D:/TG/TGProject/build/windows/bin/profile/AssetProcessorBatch.exe --project-path=D:/TG/TGProject --regset-file=D:/TG/TGProject/Gems/silpom/Tests/planar_dx12.setreg
$env:SILPOM_PLANAR_LEVEL = 'SilPOMPlanarAB_20260919'
$env:SILPOM_PLANAR_GRIDS = '32,64,128,256,512'
$env:SILPOM_PLANAR_SAMPLES = '600'
$env:SILPOM_PLANAR_RUNS = '3'
D:/TG/TGProject/build/windows/bin/profile/Editor.exe --project-path=D:/TG/TGProject -rhi=dx12 --runpython D:/TG/TGProject/Gems/silpom/Tests/planar_ab.py
python Tests/analyze_planar_ab.py D:/TG/TGProject/user/SilPOMPlanarAB/<run> --images
python -m unittest discover -s Tests -p test_planar_analysis.py
```

Image analysis requires NumPy. All captures remain at native 1920×1080, MSAA 2;
VSync is disabled only in the test session and restored afterward. The driver
requires a disposable level snapshot and rejects on-disk edits during capture.
The snapshot omits existing SilPOM entities from the **copy only**, leaving the
original DefaultLevel and the user's saved test patch unchanged. Choose a unique
snapshot name each time; this tool refuses to overwrite an existing directory.
For a short harness check use 32 samples, one run, grid 64, and
`SILPOM_PLANAR_VIEWS=front,grazing`. Such a run is **not** an acceptance benchmark.
`SILPOM_PLANAR_OUTPUT` selects a new output directory; existing directories are
refused to protect previous evidence. Default views include front, 60°, 85°, and
a deterministic moving-camera orbit. The script restores viewport and modified
CVars before closing its separate editor session.

The generator copies only into `Assets/SilPOMPlanarAB` and leaves the original
StoneWall material/textures untouched. Both variants use the same 1024×1024
single-mip linear R32_FLOAT height image, repeat addressing, UV0, and source
scale/midpoint. The manifest records source hashes and matched material values.
Grid N has `(N+1)^2` vertices and `2*N*N` triangles; the replacement has 4 and 2.

## Measurement and acceptance rules

The timestamp collector stores each distinct leaf query once per readback frame,
with raw begin/duration ticks and nanoseconds. Parent passes have no native query;
the depth leaf supplies capture cadence. Query flags are restored after capture.
The analyzer reports median/p95 for every observed leaf, missing/partial leaves,
and a sum of leaf work. **That sum is not elapsed GPU frame time**, since queues
may overlap and gaps are excluded. Do not add parent and child timings. Inspect
individual color, depth, shadow and supporting passes; do not use CPU elapsed
time or Task Manager utilization as a GPU speed claim. It also records the span
between the earliest/latest fresh graphics-queue leaf timestamps, including gaps
between those passes (not a sum). This is the main-pipeline graphics interval,
not presentation latency; it assumes the engine's uniform query readback delay.
Independent async work outside this interval must still be reviewed separately.
The actual depth attachment dimensions/MSAA and physical GPU name are recorded.
If VSync is enabled the span can include a refresh wait and must not be used as
a shader-performance claim.

Image metrics use the isolated wall's linear-depth range (0–5 m) as the ROI.
They report silhouette overlap and symmetric boundary distance, depth
median/p95/max, ROI luma SSIM (11×11 box
window), and RGB mean absolute error. This avoids allowing unchanged sky to
dominate the score. Screenshots also need visual inspection. This segmentation
must not be reused unchanged for ordinary-geometry occlusion fixtures. The latter
use a separate ordinary cube crossing the relief envelope and produce their own
screenshots/depth captures. They are excluded from isolated-wall image metrics.

Choose the *lowest sufficient* subdivision density from image/depth convergence,
not whichever gives the largest speedup. Proposed—not agreed—criteria in the
manifest are ≥20% median improvement, ≤5% p95 regression, ≤1 px p95 silhouette
distance, ≤1 mm p95 depth difference, and ROI SSIM ≥0.98. The analyzer always
keeps `accepted=false`: timing review, reference sufficiency, moving-camera
quality, nearby-geometry occlusion, shadows and agreed thresholds must be checked
before declaring the issue complete. A harness `passed=true` only means its
capture loop completed.

## Validation status

The CPU and non-DXR D3D12 conformance tests pass, as do seven analyzer unit tests.
`SilPOM` and `SilPOM.Editor`
build successfully. The regenerated main-branch Asset Processor first processed
10 assets with zero failures/errors. The optimized shader batch processed 19,
with zero failures/errors and 34 warnings (shader preprocessor locale warnings
include `Unknown encoding: C.UTF-8`). The previous recurring
transport-version error came from stale experimental `SilPOM.Builder.dll`
registration/processes, not the planar shader; the rebuilt registration no longer
loads that builder. No source assets or cache were deleted.

The initial `optimized-01` run is invalid for performance comparison: the user
edited/saved DefaultLevel and moved the test patch while it ran. It also had
VSync enabled. Its raw results are retained for diagnosis, not acceptance.
Subsequent runs use the disposable snapshot, fixed patch-position checks, and
VSync off. No GPU performance win is claimed yet.
The older [v0 validation](Validation.md) is historical evidence for the baseline,
not acceptance data for the modified coarse-shadow path.

### Isolated measurement, 2026-09-19

`user/SilPOMPlanarAB/isolated-01` completed 3 × 600 samples per variant/view,
after 120 warm-up frames plus query warm-up. AMD Radeon RX 7900 XTX, DX12,
native 1920×1080, MSAA 2, VSync off; both overlays enabled. The reference had
263,169 vertices / 524,288 triangles (512×512 cells), versus 4 / 2 for the quad.
The material and copied height hashes are in `result.json`; snapshot hash
`84cffcb6962128a26a31e8819de889e690bc5ba69196998c32db3205833c6be7` stayed fixed.

Main-pipeline graphics timestamp span, pooled 1,800 samples each, milliseconds:

| View | Quad median / p95 | Reference median / p95 |
| --- | ---: | ---: |
| Front | 3.899 / 4.576 | 3.904 / 4.971 |
| 60° | 4.226 / 4.799 | 4.062 / 5.007 |
| 85° | 5.365 / 5.809 | 3.759 / 4.620 |
| Moving orbit | 4.332 / 5.065 | 3.836 / 4.408 |

This does **not** demonstrate the required speedup. At 85°, median depth cost
was 1.557 vs 0.358 ms, and forward cost was 1.732 vs 0.752 ms. The remaining
bottleneck is per-sample height traversal, repeated in depth and forward, not
triangle submission or the coarse shadow caster. Whole-pipeline front equality
also does not imply that the relief passes are faster: front depth/forward were
0.530/0.866 ms versus 0.340/0.785 ms. All leaf statistics, including shadows and
supporting passes, are retained in `analysis.json`; no parent/child double-counting
is used. No observed pass had a partial readback series; disabled/unmeasured
passes are listed explicitly.

Image comparisons against the 512-cell reference:

| View | p95 boundary distance | p95 depth difference | ROI SSIM |
| --- | ---: | ---: | ---: |
| Front | 0 px | 0.331 mm | 0.989 |
| 60° | 0 px | 0.533 mm | 0.983 |
| 85° | 1 px | 2.183 mm | 0.948 |

Normal-budget hit masks contained zero detected invalid/exhausted pixels. Rare
interior coverage/depth outliers remain: maximum boundary distances were 21/47/4
pixels and maximum depth differences 22/56/388 mm. These are not hidden by the
p95 values and require investigation. Grazing quality fails the proposed limits;
512 cells is not certified as a sufficient reference for all views. Earlier
64/128/256/512 sweeps are in `smoke-03` and `density-01`; lower densities had larger
grazing differences. Do not inflate the reference density simply to claim a win.

Performance follow-up is tracked in
[issue #4](https://github.com/brooteskan/silpom/issues/4). It is performance-only;
quality investigation and fixes are deferred at the user's request.
The next performance work should remain planar: conservative multi-cell min/max
skipping, and evaluating whether forward shading can reuse validated per-sample
depth without changing coverage or hiding failures. Neither a curved solver nor
a reduced-resolution hit buffer is justified by these results. The local cell
range rejection alone has not established the required advantage.

Ordinary-cube occlusion captures completed for front/60°/85° in `isolated-01`.
The cube appears in front where it crosses the relief, with matching A/B
occlusion in the inspected oblique captures. These are visual checks, not an
independent all-pixel visibility oracle.

### Follow-up validation

`validation-01` completed a short 32-sample harness check, plus nine matching
moving-camera keyframes per variant, all three ordinary-cube occlusion views,
shadow attachment readbacks, and forced diagnostic/recovery checks. Its short
timings do not replace the repeated `isolated-01` measurements. Inspected motion
keyframes show corresponding relief silhouettes; they are not a continuous-video
temporal stability test because capture readback pauses the camera.

The captured 2048×2048 D32_FLOAT shadow slice was byte-identical in both modes:
SHA-256 `a4ee4c5bdfa570819f1e1655da92865085ba6b1a0e2c586e09b3e5401f798741`.
Its depth range was 0.9577049–1.0, not an empty constant image. This validates
only the captured slice, not every shadow cascade. The shader's `SHADOWMAP`
branch contains no height traversal. A one-cell traversal budget produced the
expected visible magenta patch (230,687 detected magenta pixels). Invalid width
was rejected, then restoring width recovered readiness. `analysis.json` records
the shadow hashes and diagnostic results; acceptance remains false.

The earlier `isolated-01` shutdown logged an Atom
`Shadows.Projected:SkinnedMeshes` connection error / `AuxGeomPass` configuration
assert after capture completion. An initial cleanup wait then stalled the hidden
`validation-01` Editor because background updates had already been disabled.
The driver now drains viewport/pipeline rebuilds **before** restoring the
background-update setting. `shutdown-01` repeated the short capture, restored
settings and exited without the connection error/assert or manual termination.
Two `DynamicDrawContext` pipeline-state warnings remain at shutdown; this is not
a warning-free editor smoke result. No asset build failed.

DefaultLevel and the user's saved `SilPOM_Planar_AB_UNSAVED` patch remain intact.
The final original-level hash is
`6451f865130c2f6905335369f9dc085badd9036adc5a93160b8be6ce8c7cbe9e`.
Issue #3 is **not complete**: performance gains, grazing quality/outliers,
reference sufficiency and agreed acceptance thresholds remain outstanding.

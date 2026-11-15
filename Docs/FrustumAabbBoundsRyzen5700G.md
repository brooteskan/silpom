# Frustum-clipped AABB trial on Ryzen 7 5700G

The isolated change reduces upper-glancing hit generation from **182.090 to
113.406 ms median (37.72%)**, including the new vertex-bound calculation.
The graphics-pass span falls from **296.144 to 226.412 ms (23.55%)**.
All five rendered images are pixel-identical to the frozen 2x baseline.
Both MSAA samples retain identical winning records and depth bits in all three
interior comparisons. Height-derived sampling/normals remain **on**.

Implementation: `e4dfed22c37280213b8481e0a71943859f800bff`, on
`codex/frustum-aabb-ryzen-5700g`, branched from the preserved investigation
`3623e7d592ee2074415323927071f69adaf0fb61`. The feature branch is published for
other-PC testing; main and TG's dependency pin remain unchanged.
The [compact measurements and provenance](../Tests/Reference/Ryzen5700GFrustumAabb20261115.json)
include per-repeat statistics, raw timestamp hashes and validation details.

## Change and numerical checks

[FrustumDepthBound.azsli](../Assets/Shaders/SilPOM/FrustumDepthBound.azsli)
only runs when the existing bound is 1.0. Bounds already below 1 retain the
existing calculation. It uses the **same AABB and 2e-5 local expansion**; proxy
XY coverage, displacement, estimators, sampling, pixel intersection, nearest-hit
recovery and the inset backup shell are unchanged.

The box is parameterized by a unit cube. Its six inequalities and all six
rendering-frustum inequalities (`-W <= X,Y <= W`, `0 <= Z <= W`) define the full
three-dimensional intersection. The search considers all 220 plane triples,
including newly introduced intersection vertices, rather than retaining only
original box corners. Reverse-Z depth is maximized over this enclosure.

The search proposes a depth. A separate certificate bounds the **augmented**
expression `R(q) = Z(q) - bound*W(q) + sum(weights[i]*g[i](q))` over the whole
parent unit box, using its support function to bound the residual gradient.
On the frustum intersection, `g[i](q) >= 0` and the weights are nonnegative, so
`Z - bound*W <= R <= 0`. The tighter depth inequality applies to that
**intersection**, not to offscreen portions of the unclipped box. Therefore a
skipped, nearly singular triple cannot alone authorize an unsafe result.
Frustum planes expand outward using
an absolute-product projection-error allowance (32 float epsilons plus 1e-7).
The certificate adds another arithmetic allowance (64 epsilons times its
absolute coefficient magnitude, plus 1e-7). The composed and two-stage hit
transforms are covered by the projection allowance. A candidate starts with
the existing 1e-5 depth bias; up to four increasing bias attempts must pass the
certificate. Empty, non-finite, near-plane, poorly conditioned or uncertified
cases retain 1.0. No empty-volume geometry culling was added.

[test_frustum_depth_bound.py](../Tests/test_frustum_depth_bound.py) checks the
float32 proposal against an independent float64 half-space intersection and
re-evaluates accepted certificates in float64. **860 cases pass**: 800 randomized
boxes/transforms (including thin boxes, mirrored/scaled objects and substantial
translation cancellation), three explicit near/empty cases, and the 57 captured
boxes. The review correction explicitly promotes rows, bound, weights, planes
and error operands to float64 **before arithmetic**, including both products
that previously retained float32 precision through NumPy scalar promotion.
The corrected recheck retains all 245 accepted certificates; its minimum
float64 safety slack is **7.07815e-6**. This is **CPU-only** verification: the
production shader remains unchanged and float32, including `precise float4`.
These engineering error allowances and numerical tests do not constitute an
exhaustive floating-point proof over every compiler/camera/material/transform.
CPU evaluation does not reproduce every DXIL FMA schedule.

Actual GPU post-VS output from a fresh command-line RenderDoc capture confirms
**53 of the 57** original depth-1 proxies tighten. Faces **299, 300 and 301**
retain the near-plane result; empty face **504** retains the original fallback.
Face **308** receives **0.666763067**, versus 1.0 before and a float64 geometric
maximum of 0.666559234 before outward margins. Every tightened GPU bound exceeds
its independent captured-box reference; the minimum margin is 1.51015e-5.
All 2,284 proxy vertices retain identical XY/W. Both replayed final sample-depth
planes are bit-identical to the original capture. Read-only replay completed
successfully without a device-lost error.

## Frozen fixture and visual validation

The comparison uses `Levels/SilPOMTowerBenchmark/scene.json` and its four camera
poses, with the saved tower, lighting, estimator 5, Level 2 predictor, gather,
R16 textures and **Current default** AA. Renderer policy remains
`r_silpomAntiAliasing=0`, with **2 samples**, SMAA off. The harness enables and
asserts height-derived normals at runtime without saving either level, exactly
as in the frozen corrected baseline. The serialized fixture still contains
its original normals-off field; neither that field nor the scene manifest was
changed. The effective normals-on configuration hash remains
`b76f275b9af10b14731ac7dc5115e1e29eb6edb11cebd57cd05e22b3b32ec67e`.

`TowerBenchmarkEditorSmoke.py` and `TowerBenchmarkVisualTests.py` pass.
[compare_tower_bounds.py](../Tests/compare_tower_bounds.py) compares exact UInt4
records and R32 sample-depth atlases, never a floating-point MSAA resolve:

| View | Unchanged valid winning samples, both planes | Lost/gained or changed records | Changed depth bits |
| --- | ---: | ---: | ---: |
| Lower facing | 1,843,191 | 0 | 0 |
| Lower glancing | 1,822,400 | 0 | 0 |
| Upper glancing | 1,806,350 | 0 | 0 |

All **5,471,941** valid records match, including face/UV/flags/depth. The entire
hit/depth atlas payloads also compare bit-identically: the updated comparison
retains matching SHA256 hashes and equality results under each view's
`atlas_payload_identity`, over all four atlas columns including unused ones.
Image equality now gates the overall result for **every** view. Eight regression
tests cover one-pixel changes in all five original images, real sample planes,
payload evidence and float64 cancellation cases. A real CLI negative fixture
with one changed exterior pixel and unchanged atlas inputs returns **exit 1**.
Full-payload identity is additional diagnostic evidence; semantic sample gates
cover real sample planes and valid winners, so undefined unused texels are not
treated as visible output. Exact backup record/depth
checks have zero mismatches (17 / 34,372 / 58,948 backup samples respectively).
Exterior, lower facing, both glancing views and Ctrl-G have **zero changed image
pixels**. Manual inspection confirms visible tower surfaces and an illuminated
interior column. Existing fine contour aliasing/cutouts remain identical to the
reference; this change does not repair those artifacts.

Startup identifies the Radeon GPU and SM 6.2. No new renderer binding/assertion,
shader-validation or device-lost error occurred. The pre-existing missing Yarn
`BuildCredits.ps1` error on Ctrl-G and DynamicDrawContext shutdown warnings are
also present in the baseline; the tower capture itself passes. The initial AP
connection refusal resolves when the automatically started AP becomes idle.

## Native GPU timestamps

The unchanged [2x baseline](TowerBenchmarkRyzen5700G-2f818d8.md) is retained at
`codex/benchmark-main-2f818d8`, commit `18c33bbaa1e4db68e7dfa56e47a5ecbedbea67fc`.
The trial uses the same native telemetry and capture harness. Both datasets
contain **three independent 120-frame captures per camera**, with 360 valid
hit and forward timestamps per view, 1,440 accepted frames per dataset.

Warm-up: 90 frames after setting each camera, then 120 + 30 frames before each
capture, plus the native timestamp warm-up/discard. Asset processing was idle.
Inspection was disabled throughout timing-only runs; there were no screenshots,
attachment readbacks, Ctrl-G checks or RenderDoc replays during measurement.
Reported values pool the 360 accepted frames. Units are **milliseconds**;
each pair is **median / p95**.

| View | Baseline hit generation | Frustum AABB hit generation | Median reduction |
| --- | ---: | ---: | ---: |
| Exterior | 19.278 / 19.448 | 18.588 / 18.764 | 3.58% |
| Lower facing | 79.700 / 80.175 | 59.866 / 60.387 | 24.89% |
| Lower glancing | 125.438 / 125.726 | 120.549 / 120.851 | 3.90% |
| Upper glancing | 182.090 / 187.166 | 113.406 / 115.857 | 37.72% |

| View | Baseline SILPOM forward shading | Frustum AABB SILPOM forward shading |
| --- | ---: | ---: |
| Exterior | 25.230 / 30.637 | 25.383 / 31.237 |
| Lower facing | 100.134 / 104.158 | 100.435 / 104.915 |
| Lower glancing | 105.383 / 108.948 | 105.784 / 108.790 |
| Upper glancing | 90.429 / 92.165 | 90.166 / 91.767 |

| View | Baseline graphics span | New graphics span | Baseline CPU period | New CPU period |
| --- | ---: | ---: | ---: | ---: |
| Exterior | 64.606 / 70.106 | 64.114 / 69.890 | 64.951 / 70.461 | 64.506 / 70.380 |
| Lower facing | 200.308 / 205.157 | 181.065 / 186.629 | 200.941 / 205.961 | 181.625 / 187.176 |
| Lower glancing | 251.952 / 257.524 | 248.123 / 251.447 | 252.626 / 258.266 | 248.824 / 252.088 |
| Upper glancing | 296.144 / 301.364 | 226.412 / 229.856 | 296.902 / 302.064 | 227.007 / 230.541 |

New hit-generation repeat-median ranges: exterior **18.5745–18.5980**, lower
facing **59.8525–59.8995**, lower glancing **120.5120–120.5920**, upper glancing
**113.3875–113.4440**. Baseline upper-glancing repeat medians span
182.0585–183.5230. Other metric/repeat ranges and min/max values are in the JSON.

Graphics span is the first-to-last graphics-pass timestamp interval, including
queue gaps, **not GPU busy time or FPS**. CPU period is the RHI frame-to-frame
period sampled during delayed timestamp collection, including waits; it is not
CPU busy time or a CPU/GPU frame pairing. Pass durations include the additional
vertex work. The prior baseline was preserved rather than rerun or overwritten;
small inter-session differences, especially exterior span changes, should not
be overinterpreted. Forward median changes are all within 0.61%.

At upper glancing, the largest remaining measured leaves are hit generation
**113.406 ms**, SILPOM forward shading **90.166 ms**, classify-wall-holes
**2.023 ms** and fullscreen shadows **1.213 ms**. At lower facing, forward
shading (**100.435 ms**) now exceeds hit generation (**59.866 ms**).
These are pass costs, not per-function/instruction hardware profiles.

## Matched RenderDoc invocation counters

The user-authorized retry successfully enabled Developer Mode temporarily and
collected **one warm-up plus three measured counter replays per frozen capture**.
These are separate **stable-power captured-frame replay** measurements on the
same Radeon/driver, not normal-clock application timings. The original native
tables above remain unchanged. Both individual results and the batch report
pass, with final device status **Success** and no device-lost error.

| Dominant near-field wall draw, event 473 | Original bound | Frustum AABB bound |
| --- | ---: | ---: |
| Indices / VS invocations | 3,426 / 2,284 | 3,426 / 2,284 |
| Median PS invocations | **16,878,106** | **12,294,955** |
| PS invocation repeat range | 16,877,834–16,878,162 | 12,294,755–12,295,083 |
| Passing-sample events, every repeat | 2,784,451 | 2,784,451 |
| Median replay draw duration, ms | **395.925** | **271.549** |
| Replay duration repeat range, ms | 395.812–396.379 | 271.462–271.578 |

The dominant draw executes **4,583,151 fewer PS invocations (27.15%)** and its
replay draw duration falls **31.41%**, including new vertex work. Passing-sample
events and VS invocation counts are unchanged. Passing-sample events include
overlap; they are not unique final visible pixel/sample slots.

Its **pixel-shader container bytes are identical** between the two captures.
Pixel-shader bytecode matches for all **110 corresponding draws** with a pixel shader;
only nine graphics draws have changed vertex programs. All **59/59** candidate
programs map to processed project assets using the existing read-only shader
matcher. This confirms that the bounds avoid executing unchanged pixel code,
rather than changing height sampling or the hit solver.

Across the six draws sharing the near-wall program, PS invocations fall
**18,087,350 → 12,661,682 (30.00%)** and the median per-round duration sum falls
**416.351 → 281.394 ms (32.41%)**. Across all **24 primary hit draws**, excluding
the separate wall-backup draw list, PS invocations fall
**18,310,190 → 12,884,522 (29.63%)** and duration sums fall
**416.747 → 283.525 ms (31.97%)**. Group sums are calculated within each repeat
before taking medians; parent intervals are not added to children. Other primary
hit draws together cost 0.396 ms before and 2.130 ms after in replay; their added
cost is included in the net result, not attributed to isolated vertex cycles.

The observed invocation reduction need not equal the earlier **41.8% modeled
completed-depth eligibility** reduction: these are different measurements, and
real depth availability follows draw order. Neither replay timings nor the
native graphics spans establish FPS, continuous GPU busy time, per-invocation
latency or individual shader-function costs.

The initial administrator prompt cancellation log remains preserved. On retry,
the wrapper reached its **300-second process-exit/cleanup limit after all replay
results and shader exports were saved**, stopped the still-running qrenderdoc
process, and returned exit 1. This is recorded separately from the completed,
healthy counter datasets. Its `finally` restored the prior Windows setting;
an independent registry check confirms Developer Mode's value is **absent/off**.
No further administrator prompt or counter rerun was needed to recover data.

## Review follow-up and affected-route coverage

The three review findings are corrected without changing production shader
source or adding GPU float64. The image gate now rejects differences in every
view; the CPU certificate recheck promotes every operand before arithmetic;
the augmented-expression proof and full-payload evidence are explicit above.

Matched runs also compare the original-bound shader reference at `18c33bb`
with the unchanged `e4dfed2` shader implementation at **1x, 2x and 4x**, using
the same source engine, DX12 SM 6.2, Radeon driver, 1280x720 viewport, saved
scene/lighting/estimators and runtime height-derived sampling **on**. Each mode
uses a separate startup sample-count override; Current default stays selected.
No recovery/quality controls were changed. This validates the startup sample
routes and Ctrl-G, not a live AA-selector transition or other scenes/materials.

The committed [camera path](../Tests/Reference/TowerBenchmarkNearPlanePath.json)
translates the upper-glancing camera 0.70m along its forward vector through
29 rendered steps. Eight stops have exact sample readbacks and images. The
captured-box reference for face 308 goes from reverse depth 0.327 to near-plane
contact (1.0), exercising the transition from a tighter bound to the near-plane
fallback. The four static views and Ctrl-G are also checked in each mode.

| Matched mode | Identical images | Real sample-plane readbacks | Unchanged valid records across poses | Image/record/depth differences |
| --- | ---: | ---: | ---: | ---: |
| 1x | 13 | 12 | 10,005,410 | 0 |
| 2x | 13 | 24 | 20,010,654 | 0 |
| 4x | 13 | 48 | 40,000,291 | 0 |

Every paired image and real-sample record/depth comparison passes. Complete
four-column atlas payload hashes match too. These counts span multiple camera
poses; they are not unique visible pixels. The wide exterior camera uses a 1%
minimum hit-occupancy floor because most of its image is HDRI/background;
interior/movement views retain the 25% floor, and matched equality allows zero
lost/gained samples. Editor/game sample states, normals-on controls, original
level bytes and renderer error checks pass. Existing contour artifacts remain
identical to the matching reference at each sample count.

The reference runs used a clean detached checkout; the profiling branch was
preserved and restored. AssetProcessorBatch processed 380 reference assets and
570 restored-candidate assets, with zero failures and clean readiness checks.
All 20 restored compiled hit-depth products match the pre-reference candidate
exactly, and all 82 preserved source/capture hashes remain unchanged. These are
**correctness captures**, with no new performance measurements: the original
2x timing/counter tables above are preserved.

Raw runs, comparison JSON, renderer/build logs and the one-pixel negative CLI
fixture are in `TGProject/user/silpom-review-validation-20261115`. The compact
JSON's `validation_review` contains full per-view equality evidence, source and
launch identities, and raw hashes. This extends coverage for this fixture on
this GPU/SM target; it does not establish an exhaustive guarantee for every
implicit-hit variant, compiler, transform or camera path.

## Hardware, engine and preservation

- CPU: AMD Ryzen 7 5700G, 8 cores / 16 threads.
- **Actual rendering GPU:** AMD Radeon(TM) Graphics, integrated Cezanne,
  PCI **1002:1638**, confirmed by RHI startup and RenderDoc; driver
  **31.0.21923.11000**, dated 2025-07-01 UTC.
- RAM: **32 GiB**, two 16 GiB G.Skill F4-3200C16-16GVK modules in A1/B1,
  configured at **2133 MT/s**. The iGPU shares system memory. DXGI reports
  approximately 496 MiB dedicated and 15.7 GiB shared memory in the baseline.
- DX12, matching **SM 6.2** native binaries/shader assets; viewport **1280x720**,
  **2x MSAA**, Current default, SMAA off. Desktop 1920x1200; Balanced power plan.
- Windows 11 Pro 10.0.26300; VS 2026/MSVC 19.51, CMake 4.3.1, SDK 10.0.26100
  from the retained native build. The installed SDK engine remains intact.
- Source engine `D:/engines/o3de-tg-profile`, base
  `061180bf24f1666eb30315b35da292eb14f4659c`; terrain base
  `ee6887b4b743047f7dece1b179c0cf30cb281880`.
- Bundle `tg-o3de-20261114`: LF-normalized engine patch SHA256
  `3dd9ed2e11485f7592f7462e7da850b154ca30c1159994b1ca6f0fcfec7877f3`;
  terrain patch SHA256
  `dab37b902de3e3ca417531b6b08d9fed59c20dc02382c12426b3500a89137841`.

Only shader assets changed; no native rebuild was needed. The integration
README was read and all 95 approved engine/terrain file hashes verify.
AssetProcessorBatch completed **523 assets, zero failures, zero reported errors**;
the readiness check has no failed jobs or stale products. The first compile's
reserved-identifier error and the successful retry logs are both retained.
Original `engine-integration-build.log/json` are copied beside this trial's
asset logs and describe the unchanged native build, not a fresh native rebuild.

Diversion main remains clean at **dv.commit.73**. SILPOM main/origin-main remain
`2f818d87cd29663fab360cc7ccde06e117c257cf`. TG's manifest still pins SILPOM to
`e9684244b85e878b445606164b157b16438398d4`. All **68 preserved baseline files**,
including old capture data/images, both source scene references and the
dependency manifest, retain their SHA256 hashes. No reset, merge, repin, scene
save or default change occurred. Publication is limited to the profiling branch.

## Reproduction and local raw files

Raw root: `D:/diversion/TG/TGProject/user/silpom-frustum-aabb-20261115` (ignored).

- `visuals-2x-20261115-002353`: five PNGs, exact sample atlases, logs/results.
- `capture-2x-20261115-002709`: `upper-glancing_frame303.rdc` and capture metadata.
- `probe-2x`: read-only RenderDoc post-VS vertices and both depth planes/config.
- `timings-2x-20261115-003015`: twelve raw 120-frame JSON files and summary.
- `numerical-validation.json`, `gpu-bound-validation.json`, `visual-comparison.json`,
  `timing-integrity.json`, hardware, preservation and Developer Mode verification.
- `Run-Tower.ps1`, `Capture-Tower.ps1`, counter config/cancellation log, asset
  compilation logs, and the unchanged native build log/manifest.
- `profile-baseline-retry`, `profile-candidate-retry`: full three-round counter
  profiles and shader exports; candidate shader/product matches.
- `counter-config-retry.json`, `counter-comparison-retry.json`,
  `counter-batch-retry`: batch success, timeout/restoration record and raw hashes;
  independent `developer-mode-retry-verification.json` and replay driver check.

Run from the SILPOM checkout; substitute a **fresh** ignored output path:

```powershell
$project = 'D:/diversion/TG/TGProject'
$engine = 'D:/engines/o3de-tg-profile'
$build = "$project/build/profile-5700g-sm6_2"
$python = 'C:/Users/david/AppData/Local/Programs/Python/Python312/python.exe'
& $python "$project/EngineIntegration/O3DE/apply.py" --engine $engine --project $project --verify-only
# Close Editor and asset tools before rebuilding shader assets.
& "$build/bin/profile/AssetProcessorBatch.exe" "--project-path=$project" "--engine-path=$engine" --platforms=pc
& $python Tests/MsaaAssetReadiness.py $project
& $python Tests/test_frustum_depth_bound.py --geometry "$project/user/silpom-depth-bounds-20261114/analysis-final/geometry.npz" --output "$project/user/new-numerical-validation.json"

$env:TG_SILPOM_ESTIMATOR_OUTPUT = "$project/user/new-frustum-visuals"
New-Item -ItemType Directory $env:TG_SILPOM_ESTIMATOR_OUTPUT
$env:TG_SILPOM_TOWER_SAMPLES = '2'
$env:TG_SILPOM_TOWER_RENDERDOC_VIEW = ''
$env:TG_SILPOM_TOWER_CAMERA_PATH = ''
$env:TG_SILPOM_MSAA_TIMING = '0'
$env:TG_SILPOM_TOWER_TIMING_ONLY = '0'
$editorArguments = @("--project-path=$project", "--engine-path=$engine", "--project-build-path=$build",
  '--rhi=dx12', '--regset=/O3DE/Autoexec/ConsoleCommands/r_multiSampleCount=2',
  '--runpython', "$project/Gems/silpom/Tests/TowerBenchmarkEditorSmoke.py")
$editor = Start-Process "$build/bin/profile/Editor.exe" -ArgumentList $editorArguments -WorkingDirectory $project -WindowStyle Hidden -PassThru
$editor.WaitForExit()
& $python Tests/TowerBenchmarkVisualTests.py $env:TG_SILPOM_ESTIMATOR_OUTPUT
& $python Tests/compare_tower_bounds.py "$project/user/silpom-rebenchmark-2f818d8-20261114/visuals-2x-20261114-205859" $env:TG_SILPOM_ESTIMATOR_OUTPUT --output "$project/user/new-frustum-visual-comparison.json"

$env:TG_SILPOM_ESTIMATOR_OUTPUT = "$project/user/new-frustum-timings"
New-Item -ItemType Directory $env:TG_SILPOM_ESTIMATOR_OUTPUT
$env:TG_SILPOM_MSAA_TIMING = '1'
$env:TG_SILPOM_TOWER_TIMING_ONLY = '1'
$env:TG_SILPOM_TOWER_TIMING_REPEATS = '3'
$editor = Start-Process "$build/bin/profile/Editor.exe" -ArgumentList $editorArguments -WorkingDirectory $project -WindowStyle Hidden -PassThru
$editor.WaitForExit()
& $python Tests/analyze_tower_profile.py $env:TG_SILPOM_ESTIMATOR_OUTPUT --output "$env:TG_SILPOM_ESTIMATOR_OUTPUT/summary.json"
```

The raw-root launch helpers preserve stdout/stderr, Editor logs, exit/results
and explicit engine paths. For read-only RenderDoc verification, inject the
same Editor using `renderdoccmd capture`, set `TG_SILPOM_TOWER_RENDERDOC_VIEW`
to `upper-glancing` with timing disabled, then run
`qrenderdoc --python Tests/TowerBenchmarkCoverageProbe.py` with
`TG_SILPOM_COVERAGE_CONFIG` pointing to the retained probe config (update capture
and output paths). This needs no Developer Mode. Counter collection is separate
and must not be attempted with Developer Mode off on this driver.

The counter retry used the following command. Copy the retained config and give
all output folders fresh paths before another run. The recorded 300-second
limit covers both captures and final process exit; the timeout note above
distinguishes successful data collection from the wrapper's exit status.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Invoke-TowerRenderDocProfile.ps1 -ConfigPath "$project/user/silpom-frustum-aabb-20261115/counter-config-retry.json" -TemporaryDeveloperMode -TimeoutSeconds 300
```

For the affected-route checks, use separate fresh outputs for each sample count,
timing disabled, and set the following before the normal Editor launch command:

```powershell
$env:TG_SILPOM_TOWER_CAMERA_PATH = "$project/Gems/silpom/Tests/Reference/TowerBenchmarkNearPlanePath.json"
# Set TG_SILPOM_TOWER_SAMPLES and the startup r_multiSampleCount to 1, 2 or 4.
# Keep TG_SILPOM_MSAA_TIMING=0 and TG_SILPOM_TOWER_TIMING_ONLY=0.
```

Use the same updated harness for reference/candidate runs (the retained raw-root
`harness` snapshot and `Run-Routes.ps1` do this across the detached reference).
Run `TowerBenchmarkVisualTests.py` on both outputs and `compare_tower_bounds.py`
on each matched pair. The latter checks all real sample planes selected by
metadata and returns a nonzero exit on any image difference. Run the regression
suite with `python -m unittest discover -s Tests -p test_tower_validation_regressions.py -v`.

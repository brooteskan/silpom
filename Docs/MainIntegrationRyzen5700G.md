# Main integration and Ryzen tower profile — 2026-11-16

The integration passes visual/sample validation, but the earlier interior performance gain is **not reproduced** against updated main. Lower-facing, lower-glancing and upper-glancing forward medians change by -0.17%, +0.34% and +0.20%; interior graphics spans increase by about 0.3-0.4%. These small changes do not establish a useful interior speedup.

Local main is synced to `486b178`. The separate branch `codex/integrate-main-ryzen-5700g-20261116` carries the preserved float32 angular-ambient cache onto that main. Main already contains the published frustum AABB work. No further shader algorithm changes were made during integration.

The seven initial local files are checkpointed at `8cac5f2` on `codex/preserve-angular-ambient-20261116`; their integration cherry-pick is `bf758c6`. The published frustum branch remains `cd425e4`. Main received only its authorized fast-forward; TG's dependency pin remains `e968424`. No remote changes were published.

## Hardware and frozen controls

Ryzen 7 5700G, 8 cores/16 threads; active renderer is **AMD Radeon(TM) Graphics**, Cezanne `1002:1638`, driver **31.0.21923.11000** (2025-07-01 UTC). RAM is 32 GiB, two 16 GiB G.Skill F4-3200C16-16GVK modules in A1/B1, configured at **2133 MT/s**. Windows Balanced power plan. These shared-memory APU results do not establish an ALU or bandwidth bottleneck without hardware counters.

Native Editor: DX12 **SM 6.2**, **1280×720**, **Current default AA**, 2× MSAA, SMAA off. Height-derived sampling/normals stay **on**, world-space paint baking **off**, receiver UV state **unbound**, estimator 5, R16 heights, gather validation and Level 2 predictor unchanged. The saved scene/configuration and lighting are unchanged; the harness applies the existing normals-on override without saving. Standalone SM 6.7 numerical tests also pass; native rendering/assets remain SM 6.2.

## Build and validation

Used the separate source engine `D:/engines/o3de-tg-profile`, O3DE `061180b`, terrain `ee6887b`, bundle `tg-o3de-20261115`. SDK `D:/O3DE-SDK-TG` remains intact. Compiler/tooling: VS 2026 / MSVC 19.51, CMake 4.3.1, Windows SDK 10.0.26100, four native build jobs. Required `--check-only` and `-PlanOnly` passed before building; all 91 engine and four terrain hashes pass `--verify-only` afterward. No experimental bundle flag or legacy component patch was used.

Engine patch SHA-256 (LF): `35723f2ca3cc6feb9804d43863f904e000771ee31052b6b719bb7e1130bf1256`.
Terrain patch SHA-256 (LF): `dab37b902de3e3ca417531b6b08d9fed59c20dc02382c12426b3500a89137841`.

The official native build processed 801 assets with zero failures/errors. Main and integration asset batches processed 613 and 78, also with zero failures/errors; freshness checks have no stale products or failed jobs. The official build log/JSON are retained both in `build/profile-5700g-sm6_2` and the raw evidence root. CMake's generated Vue shader copy was restored to the approved project bytes before asset processing.

All eight targeted tests pass: decal paint, height precision, receiver surface/state, UV analysis, native paint shader and directional-estimator CPU/GPU tests. SM 6.2 and SM 6.7 each pass 5,384 model queries (maximum CPU/GPU error 2.3067e-5). The existing cache oracle permits one float32 epsilon; intermediate floating-point bits are not claimed identical.

Both cohorts pass 1×, 2× and 4× visual tests: **39 images**, **84 real sample planes**, **77,414,400 depth positions**, 70,016,355 valid records, four static views, 29 movement steps with eight capture stops per AA, and Ctrl-G. Fresh main versus integration has zero changed RGBA pixels, lost/gained hits, winner-record changes or real depth-bit changes. All whole-atlas identity results are recorded in the JSON. Exterior and interior stonework render, the interior column is illuminated, and no new edge breakup is introduced relative to the frozen reference.

The older frozen reference also matches all images, UVs, hit coverage and depth. Main now allocates receiver IDs from the same counter as hit-face bases, shifting every valid historical owner ID by **+2** (`Mesh.cpp` generation setup). The original strict historical comparisons are retained as failures for this renumbering; the strict fresh main/integration comparator is unchanged.

The unsaved portable-benchmark paint fixture exercises 256 mixed dabs, erase and normal opacity, two layers and grouping off/on in gameplay. Both real samples are compared for hit, diffuse and normal output; a hidden-paint control proves visible painting. Both bake paths remain off. Details are in the reference JSON and local paint captures. No UV-bake quality claim is made. Earlier diagnostic attempts are retained: legacy VueLiveLink omits transform defaults and reports `Neural-only visibility requires trained profiles, calibrated relief depth 0.5 and DX12 MainPipeline hit reuse`; the test was moved to the approved portable scene. Camera lookup now treats authored names containing the hierarchy separator literally, and inner-wall projection normals face inward. The discarded legacy attempts also logged missing baked diffuse-probe textures; disabling their player cameras triggered Falconry `normalFov` nil errors. These attempts are not accepted baseline runs. All accepted portable runs are free of those errors. No saved scene or shader was changed for these harness corrections.

## Matched native 2× timings

Every row pools three captures of 120 frames; all required leaf timestamps are complete (360 per metric/view), with empty partial-pass lists. Camera warm-up is 90 frames, then 120 + 30 frames per repeat plus 16 native warm-up frames. Images/readbacks/inspection and asset compilation occur outside measured intervals. Both cohorts ran outside the sandbox, with no competing GPU test or RenderDoc replay. C++ and bundle inputs match between cohorts; only shader assets needed rebuilding after the source switch.

Values are **median / p95 milliseconds**. Graphics spans include queue gaps: they are not GPU busy time or FPS. CPU frame periods include waits and delayed timestamp collection, not CPU busy time or exactly paired frame costs.

| Cohort / view | Hit generation | SILPOM forward | Graphics span | CPU frame period |
| --- | ---: | ---: | ---: | ---: |
| main / exterior | 18.477 / 18.644 | 26.317 / 31.504 | 65.210 / 70.221 | 65.815 / 70.639 |
| main / lower-facing | 59.907 / 60.325 | 100.272 / 104.195 | 179.722 / 183.666 | 180.313 / 184.236 |
| main / lower-glancing | 120.549 / 120.838 | 105.950 / 109.336 | 247.723 / 251.484 | 248.340 / 252.223 |
| main / upper-glancing | 113.321 / 115.820 | 91.156 / 92.460 | 225.931 / 229.408 | 226.507 / 229.960 |
| integrated / exterior | 18.675 / 18.854 | 23.047 / 25.450 | 62.552 / 65.982 | 62.906 / 66.633 |
| integrated / lower-facing | 59.899 / 60.333 | 100.102 / 103.596 | 180.434 / 184.316 | 180.974 / 184.835 |
| integrated / lower-glancing | 120.553 / 120.850 | 106.315 / 109.797 | 248.512 / 252.619 | 249.148 / 253.290 |
| integrated / upper-glancing | 112.511 / 115.588 | 91.336 / 93.134 | 226.665 / 230.809 | 227.289 / 231.416 |

| View | Forward median change | Graphics-span median change |
| --- | ---: | ---: |
| exterior | -12.43% | -4.08% |
| lower-facing | -0.17% | +0.40% |
| lower-glancing | +0.34% | +0.32% |
| upper-glancing | +0.20% | +0.32% |

Negative changes are reductions. The JSON records every repeat-median range and the capture hashes. Exterior main forward medians range 26.014–29.729 ms, so its apparent change must be treated cautiously. These are sequential cohorts, not a randomized clock-controlled experiment. The integrated lower-glancing hit/forward medians are 120.553 / 106.315 ms, the largest remaining pass costs. Upper-glancing hit generation remains 112.511 ms with the frustum bounds. The earlier interior cache saving was not reproduced. No further optimization is proposed here.

## Logs, preservation and reproduction

A post-timing 2x control rechecks the same images and sample records after the paint test; all 13 images, hit records and real depth samples match main exactly across the four static views, 29 movement steps and Ctrl-G, ruling out changed view settings in the final timing cohort. Compiled program assets are consistent across every run within each cohort: eight ambient assets change, eight nonambient assets remain identical.

No renderer binding/assert/device-lost errors appear in the completed runs. Ctrl-G reports the pre-existing missing Yarn `BuildCredits.ps1` authoring script; shutdown DynamicDraw warnings remain. Asset import logs retain an old absolute `D:/TG` dependency-path warning, with zero failed jobs. Early visual launcher exit-code bookkeeping failed after successful fixture completion; the first 1× exit was independently observed as zero, and process-handle retention was fixed before all timing runs. The first main 2× visual exit code is unavailable; its captures, restored state and validation all completed.

TG's manifest, both saved levels, scene.json, hit-depth shader and twelve frozen timing JSONs are byte-preserved. Git expanded the frustum include from LF to CRLF on checkout; its normalized bytes and old/current Git blobs are identical. All original work, prior bundle/native files and raw captures remain under:

`D:/diversion/TG/TGProject/user/silpom-main-integration-20261116`

The root contains exact launch/switch/build scripts, source/program hashes, full PNG/DDS outputs, comparisons, 24 native timestamp captures and build/Editor logs. The compact results are [the reference JSON](../Tests/Reference/Ryzen5700GMainIntegration20261116.json).

Repeat locally with fresh timestamped outputs (close Editor/asset tools first):

```powershell
$raw = 'D:/diversion/TG/TGProject/user/silpom-main-integration-20261116'
# Switch-Source refuses dirty work, selects the pinned source, runs APBatch and checks freshness.
powershell -NoProfile -ExecutionPolicy Bypass -File "$raw/Switch-Source.ps1" -Phase main
powershell -NoProfile -ExecutionPolicy Bypass -File "$raw/Run-Validation.ps1" -Phase main -Mode routes -Samples 2
powershell -NoProfile -ExecutionPolicy Bypass -File "$raw/Run-Validation.ps1" -Phase main -Mode timings -Samples 2
powershell -NoProfile -ExecutionPolicy Bypass -File "$raw/Switch-Source.ps1" -Phase integrated
# Repeat routes at 1, 2 and 4 samples, then paint correctness, then timings at 2 samples.
powershell -NoProfile -ExecutionPolicy Bypass -File "$raw/Run-Validation.ps1" -Phase integrated -Mode routes -Samples 2
powershell -NoProfile -ExecutionPolicy Bypass -File "$raw/Run-Validation.ps1" -Phase integrated -Mode paint -Samples 2
powershell -NoProfile -ExecutionPolicy Bypass -File "$raw/Run-Validation.ps1" -Phase integrated -Mode timings -Samples 2
```

The snapshot harness under `user` is intentional: main-only controls use the same normals/bake assertions as integration. Launch arguments explicitly select the new Editor, project, build, source engine and DX12. Follow the project engine bundle README for rebuilding on another PC; do not copy this PC's DLLs or caches. Do not restore dependency pins over the preserved profiling checkout.

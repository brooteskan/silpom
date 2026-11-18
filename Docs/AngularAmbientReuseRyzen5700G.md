# AngularEdges4 ambient reuse on Ryzen 5700G

Retain the isolated float32 cache: lower-facing and lower-glancing forward medians improve by 1.51% and 1.66%, and interior graphics spans improve by 0.53–0.75% in this measured setup. The upper-glancing forward change overlaps repeat variation. All 39 rendered-image and 84 sample-plane comparisons pass.

The isolated experiment caches the four relief-top thresholds and reciprocal roots before the 32-direction ambient loop. All four caps, direction order, logistic evaluations, float32 arithmetic, per-sample shading and height-derived normals remain in use. Direct-light reach queries and the receiver decoder are unchanged.

## Retained forward draw ownership

The older 2x upper-glancing capture was replayed read-only with no counters or Windows setting changes. Ownership comes from both final integer hit-buffer sample planes and the captured material face ranges. The ranges are disjoint, cover every valid final hit, and the owned counts match the retained samples-passed counters. Timings and PS counts are the original three stable-power counter rounds; they are separate from native measurements below.

| Event | Proxy indices | PS invocations (each round) | Owned final samples | Median replay ms |
| --- | ---: | ---: | ---: | ---: |
| 2316 | 3,426 | 1,843,200 | 1,806,350 | 151.81568 |
| 2324 | 702 | 1,843,200 | 0 | 2.66272 |
| 2331 | 78 | 1,843,200 | 0 | 2.64696 |
| 2355 | 222 | 1,843,200 | 0 | 2.64328 |
| 2371 | 30 | 1,843,200 | 0 | 2.64484 |
| 2378 | 6 | 1,843,200 | 0 | 2.65608 |

Event 2316 owns all 1,806,350 valid final hit samples and accounts for 91.97% of the 165.0694 ms angular draw-duration sum. The other five angular draws own zero samples and together cost about 13.25 ms. The three neural-only forward draws also own zero samples in this capture and have a 17.62 ms median per-round duration sum. Fullscreen invocation counts therefore cannot be interpreted as complete surface-shading counts. This capture favors reducing work per owned angular sample first; the measured nonowner cost remains a separate opportunity.

## Generated code and numerical checks

The processed production angular-proxy-ambient pixel program confirms 128 dynamic reciprocal-root evaluations in the original nested 32-by-4 loop. The candidate has four reciprocal roots in the preheader and none in that loop. Both multiply instructions remain separate and ordered: angular margin times reciprocal root, then inverse width. These are DXIL observations, not AMD ISA, occupancy or register-pressure measurements.

SM6.2 and SM6.7 each pass 5,384 GPU queries against the frozen model and cap oracles (maximum CPU/GPU error 2.3067e-5). Of these, 352 compare the cached ambient integral against the original general-query loop on the same device, including random caps and thresholds 0, .5, nextafter(1,0), and 1. The explicit bound is one float32 epsilon over [0,1], 1.192092896e-7. An initial strict SM6.2 diagnostic found 27 differing float outputs, at most 5.960464478e-8 apart: the optimization is not claimed to preserve every intermediate or final floating-point bit.

## Native 2x measurements

DX12 SM6.2, 1280x720, the Radeon integrated GPU on Ryzen 7 5700G, normals on. Each cell is pooled median from three captures of 120 valid timestamps each; every required pass is complete and partial-pass lists are empty. No RenderDoc or other GPU test ran during native collection. Graphics spans include queue gaps; they are not GPU busy time or FPS.

| View | Forward baseline ms | Candidate ms | Change | Graphics baseline ms | Candidate ms | Change |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| exterior | 28.777 | 25.429 | -11.63% | 70.425 | 66.419 | -5.69% |
| lower-facing | 101.257 | 99.724 | -1.51% | 184.196 | 182.810 | -0.75% |
| lower-glancing | 106.768 | 104.990 | -1.66% | 250.823 | 249.326 | -0.60% |
| upper-glancing | 90.657 | 90.311 | -0.38% | 229.894 | 228.672 | -0.53% |

The upper-glancing forward difference overlaps baseline repeat variation and does not establish a clear pass-level gain. Full p95s, hit-generation results and repeat-median ranges are retained in [the reference JSON](../Tests/Reference/Ryzen5700GAngularAmbient20261115.json).

## Image and sample validation

The strict comparator checks every RGBA pixel, all valid winner records, and every real depth sample. No pixel is masked and no image tolerance is added. Fresh original-source controls at 1x and 2x use the same sandbox execution environment and route harness as the candidate. The 4x comparison uses the retained source-compatible reference, which already passes exactly. Each run includes four fixture views, 29 camera movement steps with eight capture stops, and gameplay.

| AA | Images compared | Changed image pixels | Real sample planes | Winner/depth changes |
| --- | ---: | ---: | ---: | ---: |
| 1x | 13 | 0 | 12 | 0 |
| 2x | 13 | 0 | 24 | 0 |
| 4x | 13 | 0 | 48 | 0 |

### Execution-environment control

The initial comparisons against older references failed in nine 1x images and nine 2x images, each at a fixed 2-by-2 pixel block (1x x=66..67, y=196..197; 2x x=578..579, y=196..197). All hit and depth data matched, and 4x images matched. This initially looked like a cache regression.

Restoring the original source and all 24 byte-identical original angular programs reproduced the same 1x block in the current sandbox: the original-code upper-glancing images matched the candidate exactly. Original-code captures outside that sandbox matched the historical reference instead. This control invalidated the initial attribution to the shader change. Fresh full 1x/2x original-code runs in the matching sandbox now pass every image and hit/depth comparison against the candidate. The historical failures remain retained as diagnostics; the low-level cause of the execution-environment discrepancy is not established.

The final candidate programs are byte-identical to the 24 products used for the timing experiment. A further outside-sandbox candidate control matches the historical upper-glancing image twice and gameplay exactly, including winner/depth equality. All scene and rendering-control fingerprints match.

The original tower and VueLiveLink level files, saved configuration, lighting, learned weights, hit shaders, defaults and engine integration hashes remain unchanged. Renderer controls are restored; runtime overrides end with the isolated Editor process.

## Reproduction and retained evidence

Raw root: `D:/diversion/TG/TGProject/user/silpom-angular-ambient-20261115` (ignored). The original/candidate production DXBC, LLVM disassembly and loop excerpts, source snapshots, original strict rounding diagnostic, GPU logs, forward probe outputs, asset build/readiness logs, both native timing sets and all historical and matched comparison results, sandbox/outside-sandbox controls, and final product-fingerprint verification are retained there. Camera-restore reminders delayed some Editor teardowns after captures; they were dismissed with reminder suppression unchecked. The first sandboxed RenderDoc startup crashed and an initial asset build was cancelled after localhost IPC denials; replay and asset compilation then ran outside that sandbox. No new counters or Developer Mode changes were requested.

Run `Tests/TowerBenchmarkForwardProbe.py` with qrenderdoc and `SILPOM_FORWARD_CONFIG` pointing to the retained `forward-config.json` (substitute a fresh output). It consumes the original profile and shader matches and validates the capture SHA256. The material offsets are intentionally scoped to that retained capture layout.

The isolated patch from starting revision cd425e4 is also retained at `Tests/Reference/AngularAmbientReuseCandidate.patch`. For numerical tests, configure standalone CMake with `SILPOM_DXC` and `SILPOM_PYTHON`, build `silpom_directional_estimator_gpu_tests`, then run `Tests/DirectionalEstimatorModelTests.py <build-dir>`. For native timings and images, use `Run-Tower.ps1` and `Run-Routes.ps1` in the raw root with fresh output names, after `AssetProcessorBatch` and `Tests/MsaaAssetReadiness.py` succeed. `Tests/analyze_tower_profile.py`, `Tests/TowerBenchmarkVisualTests.py`, and `Tests/compare_tower_bounds.py` enforce completeness and equality.

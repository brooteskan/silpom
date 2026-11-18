# Portable tower benchmark

The [2026-11-14 main rebenchmark](TowerBenchmarkRyzen5700G-2f818d8.md) records
the validated 2x project default on `2f818d8` and a separate matched 1x run.

Open `Levels/SilPOMTowerBenchmark/SilPOMTowerBenchmark.prefab` in the fixture
project `Tests/Fixtures/TowerBenchmark` (or in a project it was installed into).
The scene contains the tower at the origin, a small ground plane, a directional
sun and the engine's daylight HDRI sky/environment light. Terrain, clouds,
actors and unbaked diffuse-probe grids are omitted. The tower's authored window
area lights are retained for visible interior comparisons. The tower's SilPOM
configuration is not edited. The shader's authored spiral lighting remains enabled as saved.
Two named, unshadowed comparison fills illuminate the interior camera positions
on all lighting channels so the column edges remain visible during inspection.

Choose **Camera | Exterior (Fly Camera)** for an overall view. The other named
cameras reproduce the lower facing and lower/upper glancing views used for
edge comparisons. They are editor-only. Ctrl-G starts the exterior fly camera;
use the normal fly-camera controls to move around or through the tower.

The saved AA choice remains **Current default**. Current main controls AA
renderer-wide through `r_silpomAntiAliasing`; value 0 retains the project's
2x default. The tower component retains its estimator controls. Start comparisons at
1280×720 with the saved configuration, and record the actual GPU/driver on the
Ryzen 7 5700G machine. This scene does not impose machine-wide renderer options
or claim a measured speedup on that hardware.

## Getting the scene

The scene and every asset it uses are in the fixture project
[Tests/Fixtures/TowerBenchmark](../Tests/Fixtures/TowerBenchmark/README.md). Build
the engine with the [o3de-patches](https://github.com/brooteskan/o3de-patches)
bundle at the revision pinned in `Integration/O3DEPatches.json` (a stock engine
is insufficient), build the fixture project, build its learned bundles as its
README describes, and let Asset Processor finish before opening the scene. Use
an engine build and shader target supported by the actual GPU and driver; the
scene itself does not select or upgrade the engine's shader target. The original
visual reference used SM 6.7.

To install the frozen scene into another project that already has the tower
assets at the same project-relative paths:

```powershell
python <gem>/Tools/prepare_tower_benchmark.py --project <project>
```

It refuses to overwrite a different scene unless `--replace` is supplied.
`--generate --source-level <level> --engine <engine>` explicitly rebuilds the
frozen scene from a level that instances the tower prefab; normal installation
uses the frozen snapshot.

## Repeatable checks

`Tests/TowerBenchmarkEditorSmoke.py` opens the scene, checks the saved controls,
captures all four camera views and checks Ctrl-G rendering. Run it with the
Editor `--runpython` argument and `SILPOM_ESTIMATOR_OUTPUT` set to a fresh
output directory. It never saves either level. Optional
`SILPOM_MSAA_TIMING=1` records 120-frame GPU timestamp captures at each view;
leave that disabled for visual validation. GPU data includes the device name,
resolution and sample count. Capture and timing runs should be separate when
making performance comparisons.

On the Ryzen profiling branch, the user requires **Height-derived normals** to
remain **on** because disabling them exposes seams at mesh edges. The harness
applies and checks this setting before all views and Ctrl-G. It records the
override separately: the published `scene.json` has `UseHeightDerivedNormals`
set to false. Source levels, other tower controls and Current default AA remain
unchanged; this baseline must be reported with the normals-on correction.

The bundled source engine's main pipeline descriptor starts at 2x, which the
trained tower path does not support. For the 1x baseline, launch Editor with
`--regset=/O3DE/Autoexec/ConsoleCommands/r_multiSampleCount=1`. This is a
process-local renderer setting; the component's saved **Current default** AA
selection stays unchanged. Verify `depth_attachment.msaa == 1` in every capture.

For repeated timing-only captures, use a separate fresh output directory and set
`SILPOM_MSAA_TIMING=1` and `SILPOM_TOWER_TIMING_ONLY=1`. This skips all
screenshots, attachment readbacks and gameplay inspection. Each of the four
`scene.json` cameras receives three 120-frame captures, with inspection disabled
and a fresh warm-up before each capture. `SILPOM_TOWER_TIMING_REPEATS` may
increase the repeat count (minimum three). The native capture records GPU pass
timestamps and graphics spans. The profiling harness also samples the RHI CPU
frame-to-frame period at readback collection, including waits; delayed GPU
readbacks are not paired CPU/GPU frames. Zero means that CPU metric is unavailable.
Capture wall time includes the automation loop and is not CPU frame time.
Summarize the resulting files with
`python <gem>/Tests/analyze_tower_profile.py <timing-directory> --output <report.json>`.
The summary includes pooled medians/p95, each capture's statistics, the range of
repeat medians and the complete leaf-pass breakdown. Graphics spans can contain
queue gaps and must not be converted to GPU busy time or FPS.

Run `Tests/TowerBenchmarkVisualTests.py <capture-directory>` to check that all
views are lit and contain tower hits. The checked-in validation report records
successful editor and gameplay captures; it contains no 5700G timing claim.

The configuration and camera coordinates are retained in the neighboring
`scene.json`. Treat that configuration as the starting reference for future
optimization work.

## Ryzen 7 5700G baseline, 2026-11-14

The complete tower passes the Editor smoke test, visual validator and Ctrl-G
check on the source engine at SM 6.2. Exterior surfaces and the interior column
are visibly illuminated. Fine 1x edge aliasing remains visible in close views.
The saved levels, estimators, lighting and AA selection were not changed.
Height-derived normals are enabled at runtime as required by the user; R16
height textures and gather validation also remain enabled. This is a corrected
normals-on baseline, not an image-identical comparison with the original
normals-off reference.

The rendering device is **AMD Radeon(TM) Graphics**, PCI `1002:1638`, confirmed
by DXGI, Editor startup and timestamp captures. Driver: `31.0.21923.11000`
(2025-07-01 UTC). CPU: Ryzen 7 5700G, 8 cores / 16 threads. RAM: 32 GiB,
two 16 GiB G.Skill F4-3200C16-16GVK modules in DIMM_A1/B1, configured at
**2133 MT/s**. DXGI reports about 496 MiB dedicated video memory and 15.7 GiB
shared system memory. The integrated GPU and memory configuration constrain
cross-PC comparisons; these timings alone do not establish a bandwidth cause.
The capability probe reports SM 6.7 and native 16-bit operations, but this run
uses matching **DX12 SM 6.2** binaries/assets, **1280x720, 1 sample**, Current
default AA. Windows Balanced power plan was left unchanged.

Three separate 120-frame captures per camera follow shader/asset warm-up. There
are 360 accepted frames per view. Screenshots, hit readbacks, Ctrl-G and RenderDoc
inspection are separate. Values below are **median / p95, milliseconds**:

| View | Hit generation | SILPOM forward | Graphics pass span | CPU frame period |
| --- | ---: | ---: | ---: | ---: |
| Exterior | 10.99 / 11.11 | 13.83 / 14.32 | 40.18 / 40.71 | 40.72 / 41.31 |
| Lower-facing | 47.60 / 47.88 | 48.83 / 50.58 | 110.17 / 112.96 | 110.77 / 113.60 |
| Lower-glancing | 76.33 / 76.56 | 51.70 / 52.97 | 142.82 / 144.51 | 143.48 / 145.15 |
| Upper-glancing | 114.40 / 116.34 | 44.02 / 44.68 | 174.16 / 176.90 | 174.97 / 177.75 |

Hit generation dominates the glancing views; forward shading is comparable to
hit generation in the lower-facing view. Upper-glancing repeat medians for hit
generation range from 114.343 to 114.417 ms. Exterior forward medians vary more,
11.736 to 13.950 ms; retain that spread when comparing future runs. Other large
leaf passes are ordinary Forward (3.47 ms exterior), fullscreen shadows
(1.09-1.21 ms) and Editor outline feedback (0.86-0.87 ms). Graphics span includes
queue gaps and is neither GPU busy time nor FPS. CPU frame period includes
waits and is sampled when delayed GPU results arrive; it is not CPU busy time
or the paired CPU work for that GPU frame.

`Tests/Reference/Ryzen5700G20261114.json` contains each capture's statistics,
repeat median ranges, raw-file hashes, hardware, effective configuration and
integration provenance. Local raw evidence is retained at
`TGProject/user/silpom-profile-ryzen-5700g-20261114/`: twelve timestamp JSONs and
the pooled analysis in `timings-sm6_2-normals-on-1x-20261114-154615`, plus PNGs,
DDS readbacks and visual validation in
`visuals-sm6_2-normals-on-1x-20261114-153830`.

Inputs: Diversion `dv.commit.73`, SILPOM implementation `e968424`, O3DE
`061180b`, terrain `ee6887b`, bundle `tg-o3de-20261114`. Aggregate patch SHA-256:
engine `3dd9ed2e11485f7592f7462e7da850b154ca30c1159994b1ca6f0fcfec7877f3`,
terrain `dab37b902de3e3ca417531b6b08d9fed59c20dc02382c12426b3500a89137841`.
Native targets and AssetProcessorBatch succeeded (133 processed, zero failures
or errors); all 95 aggregate file hashes pass `apply.py --verify-only`.
Build tools: CMake 4.3.1, VS 2026 / MSVC 19.51.36260.0, Windows SDK 10.0.26100,
four build jobs. The official build log/JSON remain in
`build/profile-5700g-sm6_2`; full redirected build output is retained under the
local evidence root. The installed SDK remains intact.

Initial cache migration required another asset pass. Commit 73 supplied the
missing TerrainSkirt material type/shader; final asset readiness has no failed
jobs or stale products. No renderer binding/assert errors appeared in validated
runs. Ctrl-G reports an unrelated missing Yarn `BuildCredits.ps1` authoring
script; tower rendering succeeds, but narrative compilation is unavailable.
Startup SRG already-queued warnings and shutdown DynamicDraw warnings are
retained in logs. Neither SM 6.7 nor 4x MSAA was measured.

### RenderDoc inspection limitation

A separate upper-glancing frame was captured with official portable RenderDoc
1.47 (`b0e1370309f7d7bf986a73830446ca39d9b8c1fd`), after the twelve baseline
captures had finished. `upper-glancing_frame303.rdc` (314,466,594 bytes) and
diagnostic scripts/results remain locally in
`user/silpom-profile-ryzen-5700g-20261114/renderdoc-upper-glancing-20261114-160216`.
The capture fixture passed with normals/R16/gather enabled and saved levels
unchanged. Capture completion does **not** establish successful replay.

On this GPU/driver stack, scripted D3D12 replay exposed 13 built-in counters
and no AMD hardware counters, then returned zero counter samples and null
pixel-shader bindings. A retry using the supported blocking replay controller
gave the same incomplete results. The user reported a **GPU device lost** error
while RenderDoc was open. Task-owned replay processes were stopped. Windows
subsequently reported the device OK, with no matching Display 4101 event found;
that does not diagnose the device-loss cause. No further GPU replay was run.
The earlier baseline has no device-loss/binding/assert errors in its logs.

Consequently the measured conclusion is at pass level: hit generation is the
largest upper-glancing cost, followed by SILPOM forward shading. There is no
reliable RenderDoc instruction, source-line, occupancy or cache-stall ranking
from this attempt. Shader stepping is a debugging facility, not per-line GPU
timing. No shader edits or optimization were made.

The capture harness remains available for a separate diagnostic run: launch
Editor through `renderdoccmd capture -w -d <project> -c <fresh-output>/tower`,
followed by the same explicit Editor arguments below, with
`SILPOM_MSAA_TIMING=0`, `SILPOM_TOWER_TIMING_ONLY=0` and
`SILPOM_TOWER_RENDERDOC_VIEW=upper-glancing`. It verifies the injected API,
warms the selected scene.json camera and saves one frame. It never participates
in baseline statistics. Counter behavior is documented in RenderDoc's
[performance-counter guide](https://github.com/baldurk/renderdoc/blob/v1.47/docs/window/performance_counter_viewer.rst);
source-level stepping is described in its
[shader-debugging guide](https://github.com/baldurk/renderdoc/blob/v1.47/docs/how/how_debug_shader.rst).

Reproduce from PowerShell. `$patches` is a checkout of o3de-patches at the pinned
commit; `$gem` is this repository; the project is the fixture project:

```powershell
$gem = 'D:/src/silpom'
$project = "$gem/Tests/Fixtures/TowerBenchmark"
$patches = 'D:/src/o3de-patches'
$engine = 'D:/engines/o3de-patched'
$python = 'python'
& $python "$patches/apply.py" --engine $engine --skip terrain-compositor --check-only
powershell -NoProfile -ExecutionPolicy Bypass -File "$patches/Build-ProfilingEngine.ps1" -EnginePath $engine -ProjectPath $project -Skip terrain-compositor -ShaderModel 6_2 -PlanOnly -Python $python
# Inspect plan, close Editor/asset tools, then build:
powershell -NoProfile -ExecutionPolicy Bypass -File "$patches/Build-ProfilingEngine.ps1" -EnginePath $engine -ProjectPath $project -Skip terrain-compositor -ShaderModel 6_2 -Parallel 4 -Python $python
& $python "$patches/apply.py" --engine $engine --skip terrain-compositor --verify-only
$build = "$project/build/profile-sm6_2"
$common = @("--project-path=$project", "--engine-path=$engine", "--project-build-path=$build", '--rhi=dx12', '--regset=/O3DE/Autoexec/ConsoleCommands/r_multiSampleCount=1')
$env:SILPOM_ESTIMATOR_OUTPUT = "$project/user/tower-visual-$(Get-Date -Format yyyyMMdd-HHmmss)"
$env:SILPOM_MSAA_TIMING = '0'
$env:SILPOM_TOWER_TIMING_ONLY = '0'
$env:SILPOM_TOWER_RENDERDOC_VIEW = ''
Start-Process "$build/bin/profile/Editor.exe" -ArgumentList ($common + @('--runpython', "$gem/Tests/TowerBenchmarkEditorSmoke.py")) -Wait
& $python "$gem/Tests/TowerBenchmarkVisualTests.py" $env:SILPOM_ESTIMATOR_OUTPUT
# Review images/logs before proceeding; use a separate fresh timing directory.
$env:SILPOM_ESTIMATOR_OUTPUT = "$project/user/tower-timing-$(Get-Date -Format yyyyMMdd-HHmmss)"
$env:SILPOM_MSAA_TIMING = '1'
$env:SILPOM_TOWER_TIMING_ONLY = '1'
$env:SILPOM_TOWER_TIMING_REPEATS = '3'
Start-Process "$build/bin/profile/Editor.exe" -ArgumentList ($common + @('--runpython', "$gem/Tests/TowerBenchmarkEditorSmoke.py")) -Wait
& $python "$gem/Tests/analyze_tower_profile.py" $env:SILPOM_ESTIMATOR_OUTPUT --output "$env:SILPOM_ESTIMATOR_OUTPUT/summary.json"
```

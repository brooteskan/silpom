# Why upper-glancing hit generation costs 182 ms at 2x

The fresh RenderDoc captures establish two connected causes: **many overlapping
near-field proxies remain eligible for shading, and 2x executes that work at
sample frequency**. The dominant draw runs **16,877,906 pixel-shader
invocations**, versus **8,432,615** in a new matched 1x capture on the same
`2f818d8` implementation. Their post-VS vertex and index buffers are
byte-identical. The 2.0015x invocation increase is not a geometry increase.

That one draw accounts for approximately **95.05% of the mapped hit-generation
draw-duration sum** in 2x replay. This identifies the near-field wall hit program
as the principal investigation target, rather than the separate wall recovery
passes. No shader optimization or recovery/default change was made.

## Measurements and their scope

The unchanged normal-clock baseline on this Radeon has upper-glancing
`SilPOMHitDepth` **182.09 / 187.17 ms median/p95** at 2x, versus
**114.10 / 115.71 ms** at matched 1x. These values each have three captures
with **120 valid hit-pass timestamps per capture**, 360 total. The corresponding
forward pass also has 120 timestamps in each capture, and their partial-pass
lists are empty. This checks the review's completeness concern for the reported
upper-glancing targets; it does not rely merely on the collector's anchor count.

The investigation captured **new frames at both sample counts** using the same
revision, camera, runtime normals-on configuration, DX12 SM 6.2 and 1280x720
viewport. Each frame had one warm-up and three measured counter replays.
RenderDoc's D3D12 counters request stable power state. The following durations
are therefore **captured-frame stable-power replay values**, not application
baseline timings. They must not replace the 182 ms result or be called FPS,
continuous GPU busy time, or individual shader-invocation latency.

| Dominant near-field draw | 1x | 2x |
| --- | ---: | ---: |
| Event | 464 | 473 |
| Indices / instances | 3,426 / 1 | 3,426 / 1 |
| VS invocations | 2,284 | 2,284 |
| Median PS invocations | **8,432,615** | **16,877,906** |
| PS invocation repeat range | 8,432,615–8,432,839 | 16,877,778–16,878,218 |
| PS invocations per viewport sample | 9.1500 | 9.1569 |
| Median draw duration, ms | **246.42** | **396.01** |
| Draw-duration repeat range, ms | 245.888–247.599 | 395.738–396.013 |
| Passing sample events | 1,392,377 | 2,784,451 |
| Unique samples whose depth changed | 902,414 | 1,804,749 |

The 2x dominant draw's PS count is about **18.31 invocations per viewport
pixel**, or **9.16 per viewport sample**. Invocation and passing-sample counters
are not unique visible pixels. The depth readbacks separately show 903,085
pixels with at least one changed sample at 2x. Misses/discards, depth outcomes
and helper execution are not separately attributed by these counters.

Program matching assigns **24 draws** to the `silpomHitDepth` draw list in each
capture. Their per-round duration sums are 416.356, 416.618 and 416.649 ms at
2x; 258.670, 260.375 and 259.194 ms at 1x. The ratio of medians is **1.6074x**,
close to the normal-clock hit-pass ratio **1.5959x**. The dominant draw's replay
ratio is **1.6071x**. This is consistent with the sample-work multiplier, but
does not establish a constant per-invocation cost: the two sample counts select
different compiled programs and execution/cache behavior can differ.

Per-round sums precede all reported group medians. Wall-backup's separate draw
list is excluded from the hit group. No parent intervals are added to children,
no normal-clock graphics-span medians are subtracted to invent an overhead,
and replay shares are not used to allocate exact milliseconds of the 182 ms
normal-clock pass.

## Which shader and why it executes so much

All **59** captured 2x shaders and **54** captured 1x shaders match processed
project programs. Matching uses exact container/DXIL bytes or the established
in-memory reconstruction of O3DE specialization, excluding only the re-signed
container hash. No processed asset is modified.

The 2x dominant pixel shader, `ResourceId::3001`, maps to
`silpommesh_mainpipeline_silpomhitdepthlodnearwall_dx12_0.azshadervariant`.
Its six draws sum to **416.22 ms median**. Three other near-field draws use the
generic near program (0.129 ms); shell and learned-proxy programs are also small
(0.221 and 0.047 ms). The separate wall-backup program is **2.306 ms** across
nine draws. Those replay costs show that the expensive work is the primary
near-field intersection draw, not the restored backup policy.

The captured 2x disassembly declares **`SV_SampleIndex`** and a sample-interpolated
UV input. Its output is **`SV_DepthLessEqual`**. The captured PSO uses reverse-Z
**GreaterEqual**, depth writes, no culling, no stencil test, no depth-bounds test,
full sample mask and a two-sample D32S8 target. The 1x shader is the NoMSAA
program, `ResourceId::3175`; its near and near-wall asset names alias the same
program. The 2x static-wall supervariant hardcodes Level 2 and gather to true;
the 1x program specializes both options to 1. Height-derived sampling stays on.

Microsoft specifies that a sample-index input forces sample-frequency
execution, and covered samples then get separate shader outputs. This is
confirmed here by the nearly doubled hardware invocation count, not inferred
only from the attachment's sample count.
[Sample-frequency execution specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm).

The unchanged source route is:

1. [EvaluateVertexGeometry](../Assets/Materials/Types/SilPOMMesh/SilPOMMesh.azsli)
   bounds the displaced authored face and creates a conservative projected
   triangle/quad. Glancing faces produce overlapping candidate footprints.
2. [SilPomNearestFaceDepth](../Assets/Materials/Pipelines/MainPipeline/SilPOMHitDepth.azsli)
   gives every vertex of each proxy the same nearest-depth bound, using the
   expanded face AABB's `maxZ/minW`, saturated or returned as 1.0 near the camera.
3. The pixel stage evaluates the ray/face volume, predictor, validation and
   approved nearest-hit recovery through
   [SilPomIntersectFace](../Assets/Shaders/SilPOM/BlendedFace.azsli). Misses can
   discard; winners publish actual depth and ownership records. These operations
   happen inside each invocation. They do not create new PS invocations by
   themselves.

Conservative depth can preserve early rejection. It would be incorrect to say
the shader simply disables early Z. The limitation is the **weak conservative
bound**: reverse-Z raster depth 1.0 passes against every normalized stored depth
in this capture, leaving the expensive intersection to decide the actual hit.

## Geometry and depth evidence

The dominant draw contains **571 four-vertex proxies**; **172** overlap the
viewport. The exported buffers are byte-identical between the two sample counts.
For both captures, each proxy's four raster depths are identical.

The CPU model uses top-left rasterization, 8-bit subpixel snapping and the
captured before/after D32S8 samples. The 2x model uses the Direct3D standard
sample positions (+4,+4), (-4,-4) in sixteenths relative to pixel center.
The capture has quality zero; this is an explicit standard-pattern assumption,
not a queried vendor location table. It does not model in-draw depth evolution,
hardware Hi-Z granularity or helper lanes. Its counts describe candidate work,
not exact hardware invocations.
[Microsoft standard sample-pattern diagram](https://microsoft.github.io/DirectX-Specs/d3d/archive/images/d3d11/D3D11_3_MSAAPatterns_2_4.png).

| Coverage/bound model | 1x | 2x |
| --- | ---: | ---: |
| Potential proxy-covered sample positions | 11,365,822 | **22,733,080** |
| Average candidates per viewport sample | 12.33 | 12.33 |
| Maximum / p95 overlap | 36 / 26 | 36 / 26 |
| Eligible against depth before the draw | 11,339,828 | **22,680,953** |
| Eligible against completed draw depth | 7,219,461 | **14,440,274** |
| Nonempty proxies with depth bound 1.0 | 57 | 57 |
| Coverage from those depth-1 proxies | 6,224,491 | **12,449,726** |

The observed 16.88-million 2x invocation count is between the before/completed
depth estimates. Even completed closest depth leaves about 14.44 million
modeled candidates. This supports an overlap/bound problem beyond draw ordering
alone. It does not measure how many cycles prediction, validation or texture
accesses consume within each invocation.

The local [coverage plot](../../../user/silpom-renderdoc-msaa2-20261114/coverage-2x/sample-coverage.png)
shows mean overlap across both sample planes and the candidates surviving the
completed-depth comparison. Separate readbacks were required and exported for
both samples; generic Atom MSAA readback was not used.

## Limits and preserved baseline

RenderDoc 1.47 replay succeeded without degradation or device loss on AMD
Radeon(TM) Graphics, PCI 1002:1638, driver 31.0.21923.11000. GPUPerfAPI still
reports **Hardware Not Supported**. Built-in duration, PS/VS invocation and
passing-sample counters work, but there are no AMD occupancy, memory-stall or
cache-miss counters. Captured shaders have no debug source files. Static DXIL
operation sites are not runtime execution counts. No per-function or per-line
cost ranking is claimed.

The investigation is isolated on `codex/renderdoc-msaa2-ryzen-5700g`, starting
from benchmark/report commit `18c33bb`. The main branch remains `2f818d8`.
Saved scene, scene.json, TG pin, baseline report and all **24 raw normal-clock
captures** verify unchanged. No native rebuild, shader source change, material,
lighting, estimator, nearest-hit recovery or saved AA change was made. The 1x
diagnostic uses only a process-start sample-count override.

The counter wrapper temporarily enabled Developer Mode under the existing
authorization, then restored the originally absent registry value. An independent
registry read verifies it absent again. Bindless feedback was disabled only in
the replay process to avoid the previously isolated instrumentation failure;
no persistent RenderDoc configuration was saved. All shader mapping and depth/
geometry readbacks occurred outside counter intervals.

Raw frames, complete counter rounds, 113 matched shader exports, both samples'
depth buffers and analysis remain local at
`TGProject/user/silpom-renderdoc-msaa2-20261114/`. Compact results and hashes:
[Ryzen5700GRenderDocMSAA2-20261114.json](../Tests/Reference/Ryzen5700GRenderDocMSAA2-20261114.json).

## Command-line reproduction

The saved frames are `capture-2x-20261114-220921/upper-glancing_frame303.rdc`
and `capture-1x-20261114-221142/upper-glancing_frame303.rdc`. Their adjacent
`results.json` files verify scene/configuration, AA and normals-on controls.
To make fresh frames, use the RenderDoc capture procedure in
[TowerBenchmark.md](TowerBenchmark.md), setting `SILPOM_TOWER_SAMPLES=2`
and startup `r_multiSampleCount=2` for the default, then 1 for the separate
comparison. Set `SILPOM_TOWER_RENDERDOC_VIEW=upper-glancing`, timing flags to
0 and a fresh ignored output folder. Do not change the saved scene.

The batch profile configuration supports two saved captures under one temporary
Developer Mode session. Re-run in a fresh output folder:

```powershell
$project = 'D:/diversion/TG/TGProject'
$repo = "$project/Gems/silpom"
$raw = "$project/user/silpom-renderdoc-msaa2-20261114"
$rd = "$project/user/silpom-profile-ryzen-5700g-20261114/tools/RenderDoc-1.47/RenderDoc_1.47_64"
$output = "$project/user/renderdoc-msaa2-repeat-$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory -Path $output | Out-Null
$config = Get-Content "$raw/counter-config-retry.json" -Raw | ConvertFrom-Json
$config.output = "$output/state"
foreach ($item in $config.profiles) { $item.output = "$output/profile-$($item.samples)x" }
$config | ConvertTo-Json -Depth 8 | Set-Content "$output/config.json"
powershell -NoProfile -ExecutionPolicy Bypass -File "$repo/Tools/Invoke-TowerRenderDocProfile.ps1" -ConfigPath "$output/config.json" -TemporaryDeveloperMode -TimeoutSeconds 600
if ($LASTEXITCODE -ne 0) { throw 'Inspect profile and restoration state' }
# Native administrator prompt requires user interaction; no desktop automation.
```

`TowerBenchmarkCoverageProbe.py` takes `SILPOM_COVERAGE_CONFIG` containing
capture, capture_metadata, output, samples, event and depth_readbacks=true.
Use event 473 for this 2x frame, 464 for this 1x frame. `auto_indices=3426`
also validates the conservative-depth candidates; ambiguous candidates require
an explicit event. Launch it via `qrenderdoc --python`, with hidden window and
fresh output. It requests no counters or Windows setting changes. Full input
configurations are retained in `probe-2x/config.json` and
`probe-1x-event464/config.json`.

```powershell
$python = 'C:/Users/david/AppData/Local/Programs/Python/Python312/python.exe'
& $python "$repo/Tests/match_renderdoc_tower_shaders.py" "$raw/profile-2x/profile.json" "$project/Cache/pc" --output "$output/shader-matches-2x.json"
& $python "$repo/Tests/analyze_tower_msaa_coverage.py" "$raw/probe-2x" --output "$output/coverage-2x"
& $python "$repo/Tests/analyze_tower_msaa_coverage.py" "$raw/probe-1x-event464" --output "$output/coverage-1x"
# NumPy required; optional --plot uses Matplotlib and saves a standalone PNG.
```

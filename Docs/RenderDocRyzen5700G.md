# Ryzen 5700G RenderDoc investigation, 2026-11-14

The command-line workflow now produces usable **per-draw GPU timings,
invocation counts, captured shader bytecode and source-program identities**.
One warm-up and three measured replays completed; all 186 timed draws and
dispatches were mapped with a successful final replay status. All 54 captured
shaders matched processed project shader programs.

The original baseline remains at `8efd8e95159fd3633cf085d2f8ce44f77c526ff6` on
`codex/profile-ryzen-5700g`. This follow-up is on
`codex/renderdoc-ryzen-5700g`. Its normal-clock timestamps, report, source scene,
lighting, AA selection, dependency pins and shader implementation are unchanged.
No shader optimization, shader recompilation, driver update or Windows timeout
change was made. Height-derived normals, R16 heights and gather sampling remain
**on**, verified from the capture's fixture metadata. Current default AA is 1x,
1280x720, DX12 SM 6.2. The rendering device is AMD Radeon(TM) Graphics,
PCI `1002:1638`, driver `31.0.21923.11000`.

## Two replay failures isolated

1. **Counter collection required Windows Developer Mode.** A healthy validated
   replay exposed shader bindings, but requesting even the single GPU-duration
   counter produced an explicit fatal status: D3D12 counters require Developer
   Mode. RenderDoc 1.47 calls `SetStablePowerState(TRUE)` unconditionally in its
   D3D12 counter path. Microsoft documents device removal when that API is used
   with Developer Mode off. This explains the initial empty counter results and
   null bindings: the old script continued using a controller after a fatal
   error. It was not evidence of a missing shader or a baseline renderer failure.
2. **Optional bindless-descriptor feedback failed during inspection.** After the
   counter rounds succeeded, selecting event 2294 with feedback enabled caused
   `E_INVALIDARG`, followed by `DXGI_ERROR_DEVICE_REMOVED` while RenderDoc read
   its feedback buffer. With the process-local setting
   `D3D12.BindlessFeedback=false`, the same capture mapped all 186 draws and
   dispatches successfully. This isolates the failure to the instrumented
   feedback path on this stack; it does not establish the underlying driver or
   RenderDoc implementation defect. Resource-usage feedback is unavailable in
   this mode; shader identities, reflection and timing counters remain usable.

With the user's approval, `AllowDevelopmentWithoutDevLicense` was temporarily
set to 1 for the counter runs. Its original state was an absent value; cleanup
restored that state and a separate registry read verified it. Developer Mode is
**off again**. Only this development setting was changed. The successful
wrapper returned zero, and `developer-mode-state.json` records `restored:true`.
The replay's stable power request is released before shader mapping.

Primary references:
[Microsoft SetStablePowerState contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-setstablepowerstate),
[RenderDoc 1.47 counter implementation](https://github.com/baldurk/renderdoc/blob/v1.47/renderdoc/driver/d3d12/d3d12_counters.cpp),
[RenderDoc bindless-feedback switch](https://github.com/baldurk/renderdoc/blob/v1.47/renderdoc/driver/d3d12/d3d12_shader_feedback.cpp).

## Measured shader costs

These are **stable-power captured-frame replay measurements**, not the original
normal-clock baseline. Durations are sums of each shader's events within each
replay, followed by the median and range across three replays. The denominator
for shares is the sum of 225 timed-event intervals: median **378.03 ms**,
range **377.02-379.11 ms**. Such intervals can overlap across queues and exclude
queue gaps; their sum is neither frame span nor continuous GPU busy time/FPS.

| Captured pixel shader | Draws | Median sum, ms | Repeat range, ms | Share of event-duration sum |
| --- | ---: | ---: | ---: | ---: |
| Near-field hit depth, `ResourceId::3172` | 9 | **260.78** | 259.79-261.34 | **68.98%** |
| Angular-proxy ambient forward, `ResourceId::3198` | 6 | **84.61** | 84.57-84.71 | **22.38%** |
| Neural-only forward, `ResourceId::3183` | 3 | 6.82 | 6.82-6.83 | 1.80% |
| Fullscreen shadow, `ResourceId::1098` | 1 | 3.41 | 3.41-3.42 | 0.90% |

The largest individual hit-depth draw, **event 464**, takes **248.05 ms** median
(247.08-248.62 ms) and reports about **8.43 million pixel-shader invocations**
for a 921,600-pixel viewport: about **9.15 invocations per viewport pixel**.
That is an invocation/workload observation, not an exact unique-pixel overdraw
measurement or individual shader-invocation latency. Across the nine hit-depth
draws there are about 9.03 million PS invocations per replay.

The largest forward draw, **event 2294**, takes **79.50 ms** median
(79.46-79.60 ms), with **921,600 PS invocations**. Six draws using this forward
program each report that invocation count. Their total time is much smaller
than hit generation, while the large individual forward draw still accounts
for substantial cost. Other mapped shader programs are listed in
`Tests/Reference/Ryzen5700GRenderDoc20261114.json`.

The measured bottleneck is the near-field hit-depth program. Its source route
is [SilPOMHitDepthLodNear](../Assets/Materials/Pipelines/MainPipeline/SilPOMHitDepthLodNear.azsli)
through the neural-cache/no-march depth pipeline. The 1x near-wall wrapper
produces identical program bytes, so both generated asset names match. The
existing Level 2 predictor, gather validation and approved nearest-hit recovery
remain present. The other major program maps to
[SilPOMHitForwardAngularProxyAmbient](../Assets/Materials/Pipelines/MainPipeline/SilPOMHitForwardAngularProxyAmbient.azsli),
which selects angular-edge handling, diffuse accessibility and shared proxy
lighting. These are program-level associations, not timings for individual
functions inside either program.

Shader identity matching reproduces O3DE's runtime specialization encoding on
**in-memory copies** of the processed bytecode. It then checks exact program
bytes, excluding only the re-signed container hash. Exact unmodified container
or DXIL hashes are used where possible. This resolved all 54 shaders without
editing any asset. The captured hit-depth program specifically contains
specialized gather and Level 2 values of 1.

## Limits and evidence

GPUPerfAPI explicitly reports **Hardware Not Supported** for this Radeon in
the RenderDoc log. No AMD occupancy, cache-miss or memory-stall counters were
available. Captured shaders have no HLSL debug files. DXIL disassembly and static
operation-site counts are retained locally, but branches, loops and compiler
lowering prevent treating those counts as runtime work or a cost explanation.
There is **no per-line or per-instruction GPU cost claim** here.

The unmodified baseline remains the reference for normal application timings:
its upper-glancing hit-generation pass is 114.40/116.34 ms median/p95 and SILPOM
forward is 44.02/44.68 ms. Stable-power replay can reduce clock rates, and
RenderDoc adds instrumentation, so these new durations must not be used to
claim a regression, speedup or FPS change against that baseline.

Raw evidence is local under
`TGProject/user/silpom-profile-ryzen-5700g-20261114/`:

- `renderdoc-upper-glancing-20261114-160216/upper-glancing_frame303.rdc`:
  original 314,466,594-byte frame, captured with height-derived sampling on.
- `renderdoc-investigation-20261114-184351/integrated-profile/`: successful
  `profile.json`, three full counter rounds, per-draw shader mapping, exported
  bytecode/disassembly, exact source-product matches and restoration record.
- `noopt-validation-duration` within that investigation: explicit initial
  Developer Mode fatal status and log.
- `developer-mode-profile`: successful initial counter rounds followed by the
  feedback-path failure; retained separately from the final successful run.
- `preflight-mode-off`: verified safe rejection before opening a replay or
  requesting counters when Developer Mode is off.

The compact checked-in JSON records raw hashes, repeats, largest draws and
program identities. Native baseline files and the baseline branch hash were
verified unchanged after the investigation.

## Reproduce through command-line tools

Use the existing normals-on capture, or make another separate inspection frame
with the capture harness described in `TowerBenchmark.md`. No Editor or asset
rebuild is needed to replay the saved frame. The portable tool is official
RenderDoc 1.47, commit `b0e1370309f7d7bf986a73830446ca39d9b8c1fd`.

```powershell
$project = 'D:/diversion/TG/TGProject'
$repo = "$project/Gems/silpom"
$run = "$project/user/silpom-profile-ryzen-5700g-20261114"
$rd = "$run/tools/RenderDoc-1.47/RenderDoc_1.47_64"
$captureFolder = "$run/renderdoc-upper-glancing-20261114-160216"
$output = "$project/user/renderdoc-profile-$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory -Path $output | Out-Null
$configPath = "$output/config.json"
@{
    capture = "$captureFolder/upper-glancing_frame303.rdc"
    capture_metadata = "$captureFolder/results.json"
    output = $output
    qrenderdoc = "$rd/qrenderdoc.exe"
    repeats = 3
    shader_export = $true
} | ConvertTo-Json | Set-Content -LiteralPath $configPath
# Explicitly opts into the temporary machine-wide development setting.
# Accept the administrator prompt; the wrapper restores the prior value in finally.
powershell -NoProfile -ExecutionPolicy Bypass -File "$repo/Tools/Invoke-TowerRenderDocProfile.ps1" -ConfigPath $configPath -TemporaryDeveloperMode
if ($LASTEXITCODE -ne 0) { throw 'Inspect profile.json and developer-mode-state.json' }
python "$repo/Tests/match_renderdoc_tower_shaders.py" "$output/profile.json" "$project/Cache/pc" --output "$output/shader-product-matches.json"
```

Omit `-TemporaryDeveloperMode` only if Developer Mode is already enabled. The
Python profiler refuses counter collection while it is off, preventing the
original device-removal call. It checks fatal replay status after every counter
round and frame-event change. Descriptor feedback is disabled only in that
analysis process and restored before exit; no persistent RenderDoc configuration
is saved. Screenshots, shader exports and state inspection are outside counter
intervals. The wrapper's four-minute deadline also runs restoration on timeout.

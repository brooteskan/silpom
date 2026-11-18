# Why the upper-glancing hit-depth draw invokes 8.43 million pixel shaders

The dominant draw shades **overlapping conservative proxies for authored faces**.
Each screen pixel can belong to several proxies, and the raster depth often
cannot rule those candidates out before the shader determines the actual relief
intersection. This explains the invocation count's scale without changing any
shader or sampling setting.

This is draw **464**, captured vertex/pixel shaders `3171/3172`, from the
unchanged upper-glancing frame used for the RenderDoc report. Height-derived
normals, R16 heights and gather sampling are **on**. It uses 1280x720, DX12
SM 6.2, one sample, one instance, a full-viewport scissor, 1x1 shading rate,
no face culling and hardware conservative rasterization disabled. Thus the
"conservative" coverage discussed here comes from SILPOM's proxy geometry.

## Captured geometry and depth evidence

The draw has 2,284 vertices and 3,426 indices: **571 four-vertex proxies,
1,142 raster triangles**. The captured VS output carries 571 distinct authored
face indices, with one face index shared by each proxy's four vertices. The
existing hardware counters also report 2,284 VS invocations.

Reconstructing pixel-center raster coverage from those actual VS positions gives:

| Quantity | Result |
| --- | ---: |
| Proxies covering viewport pixels | **172 / 571** |
| Sum of proxy raster footprints, before depth tests | **11,365,822** pixel positions |
| Footprints divided by 921,600 viewport pixels | **12.33x** |
| Overlap at a viewport pixel, median / p95 / maximum | **12 / 26 / 36** |
| Proxies covering at least half the viewport | **0** |
| Proxies with exact conservative raster depth 1.0 | **57** contributing proxies |
| Coverage contributed by those depth-1 proxies | **6,224,491** pixel positions |

The largest proxy covers about 42.4% of the viewport. The workload is distributed
among many overlapping bounds; no individual proxy covers the whole viewport.

Depth was read back after the previous draw, event 460, and after event 464.
The D32S8 target is 1280x720 with one sample. Its depth increases monotonically
as expected for the captured reverse-Z `GreaterEqual` test and enabled writes.

| Raster-depth comparison applied to the proxy footprints | Eligible pixel positions |
| --- | ---: |
| Against depth before the draw | **11,339,828** |
| Against the completed draw's depth | **7,219,461** |
| Actual measured PS invocations, median of three replays | **8,432,615** |

The actual three PS counts are 8,432,359, 8,432,775 and 8,432,615: about
**9.15 invocations per viewport pixel**. They fall between the model's eligibility
counts. Even if the completed closest-depth image were available at the start,
about **7.22 million** proxy-covered positions would still pass the conservative
bound test. This shows why the issue is broader than primitive ordering alone.
It is not an exact simulation of the GPU's hierarchical depth behavior.

The draw reports 1,392,377 passing sample events, while the readbacks show
902,414 unique pixels whose depth changed. Passing sample events can include
repeated updates or equal-depth results at a pixel; they are not unique visible
pixels. The gap from PS invocations includes hit misses/discards and depth
rejection, without separately measuring each reason.

## How the unchanged shader produces this workload

1. [EvaluateVertexGeometry](../Assets/Materials/Types/SilPOMMesh/SilPOMMesh.azsli)
   projects six extrema per authored face: its three corners at both relief
   height limits. It clips that volume to the near/far and near-field limits,
   then chooses a conservative screen-space triangle or quad. In this glancing
   view, neighboring displaced-face bounds overlap heavily.
2. [SilPomNearestFaceDepth](../Assets/Materials/Pipelines/MainPipeline/SilPOMHitDepth.azsli)
   assigns a constant nearest-depth bound to every vertex of a proxy. It uses
   the expanded face AABB's `maxZ/minW`, returning or saturating to 1.0 when the
   bound reaches the camera/near-depth limit. All four vertices of every
   captured proxy have the same raster depth.
3. The compiled pixel shader outputs **`SV_DepthLessEqual`**; captured depth
   testing is **`GreaterEqual`**. Conservative depth can preserve early rejection,
   so it would be incorrect to say this shader simply disables early Z. But a
   proxy raster depth of **1.0** passes the conservative bound test against any
   normalized depth in this capture. Its actual relief intersection still has
   to be determined before the real depth winner is known.
4. Pixel work then performs ray/volume tests, prediction, validation and the
   approved nearest-hit recovery. [SilPomIntersectFace](../Assets/Shaders/SilPOM/BlendedFace.azsli)
   may return a miss; the material shader discards misses and out-of-clip hits.
   Otherwise it publishes the actual hit depth and record. These operations
   happen inside a PS invocation. Predictor/validation loops affect the cost
   per invocation, not how the pipeline counts new invocations.

The measured explanation is therefore **many overlapping candidate proxies plus
weak early-depth rejection from conservative nearest bounds**. Height-derived
sampling was left enabled throughout; no off/on comparison, recovery change,
estimator change, AA change or shader optimization was made.

## Method, limits and reproduction

RenderDoc 1.47 was driven through `qrenderdoc --python`; optional bindless feedback
was disabled only for each diagnostic process, using the established workaround.
No new timing counters were requested, so **Developer Mode stayed off** and no
Windows setting changed. The existing three counter rounds were reused. Post-VS
mesh and depth readbacks are separate diagnostics, outside measured intervals.
Repeated mesh extractions were byte-identical.

The CPU analysis uses 8-bit subpixel snapping and the top-left rule. It validates
the captured no-cull, one-sample, constant-proxy-depth conditions. The eligibility
counts do not model in-draw depth evolution, hardware Hi-Z granularity or helper
lanes. They explain the workload's scale; the 8.43-million hardware counter
remains the measured execution count. No exact attribution of the remaining
CPU/GPU difference, per-function costs or discard reasons is claimed.

References: [D3D pipeline statistics](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_query_data_pipeline_statistics),
[HLSL conservative-depth semantics](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-semantics),
[Direct3D rasterization rules](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm).

Raw data, the complete per-proxy breakdown and the overlap plot are local at
`TGProject/user/silpom-profile-ryzen-5700g-20261114/coverage-investigation-20261114-194118/`.
`verified-probe/` contains the state JSON, post-VS buffers and before/after D32S8
readbacks. `analysis/proxy-coverage.png` shows footprints and candidates surviving
comparison with completed depth. The compact checked-in record is
`Tests/Reference/Ryzen5700GCoverage20261114.json`.

```powershell
$repo = '<gem>'
$project = "$repo/Tests/Fixtures/TowerBenchmark"
$run = "$project/user/silpom-profile-ryzen-5700g-20261114"
$capture = "$run/renderdoc-upper-glancing-20261114-160216"
$rd = "$run/tools/RenderDoc-1.47/RenderDoc_1.47_64"
$output = "$project/user/coverage-$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory -Path $output | Out-Null
$env:SILPOM_COVERAGE_CONFIG = "$output/config.json"
@{
    capture = "$capture/upper-glancing_frame303.rdc"
    capture_metadata = "$capture/results.json"
    output = $output
    event = 464
    before_event = 460
    depth_readbacks = $true
} | ConvertTo-Json | Set-Content -LiteralPath $env:SILPOM_COVERAGE_CONFIG
$probeScript = "$repo/Tests/TowerBenchmarkCoverageProbe.py"
Start-Process "$rd/qrenderdoc.exe" -ArgumentList @('--python', ('"'+$probeScript+'"')) -WorkingDirectory $rd -WindowStyle Hidden -Wait
python "$repo/Tests/analyze_tower_proxy_coverage.py" $output --output "$output/analysis"
# Optional: add --plot when matplotlib is installed; NumPy is required for analysis.
```

The baseline branch remains `codex/profile-ryzen-5700g` at `8efd8e9`, and the
RenderDoc profile branch remains at `28845c9`. This coverage follow-up is isolated
on `codex/coverage-ryzen-5700g`. No scene, shader, native renderer, dependency pin,
baseline capture or baseline report was changed.

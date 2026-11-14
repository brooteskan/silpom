# StoneWall pixel shader performance

Follow-up to recommendation 1 in the RX 7900 XTX investigation for
[terrain-compositor #17](https://github.com/brooteskan/terrain-compositor/issues/17).
Measured September 24, 2026, against SilPOM commit
`6017e36fd5847a382030021c94d1e0f18fff62e9` and the existing TG Editor build.

## Result

The expensive far-view draws belong to the saved **`silpom test`** entity,
using `Assets/SilPOM/stonewall.material` and its 1024 × 1024 height image.
An exact power-of-two texel-addressing optimization reduces the matched depth
and forward draw cost by **about 42%**. The stationary far-view native GPU
frame median falls from **4.10–4.13 ms to 3.55–3.57 ms**, approximately
**0.55–0.56 ms / 13–14%**. Same-frame color and linear depth are byte-identical.

Whole-frame gains are established for the stationary far view in these runs.
Other views had different GPU clock distributions and higher optimized-run
frame times; they do not establish an across-view frame-time improvement.
See the full measurements and limits below.

## Attribution and change

Temporarily disabling `silpom test` reduces native far GPU time from 4.123 to
3.234 ms; restoring it returns 4.114 ms. Disabling
`SilPOM_Planar_AB_UNSAVED` instead gives 4.112 ms. The level is never saved.

The original capture's expensive six-index draws use pixel shaders
`{f3392722}` (depth) and `{0a876938}` (forward). They disappear when the
StoneWall patch is disabled. The corresponding vertex shaders remain
`{d3733b27}` and `{4bd5d990}` after optimization; their runtime bytecode matches
the original exactly. Optimized pixel shader hashes are `{855e1fcc}` and
`{70d3134e}`. Shader hashes are capture identifiers, not source-file hashes.

The patch is 2 × 2 m, with 0.2 m displacement scale, reference height 0.5,
repeat addressing and a 4096-cell budget. Its saved configuration uses the
existing frozen traversal path. Each height coordinate previously evaluated
`((i % size) + size) % size`. In
[Heightfield.azsli](../Assets/Shaders/SilPOM/Heightfield.azsli), positive
power-of-two dimensions now use `uint(i) & uint(size - 1)`. This computes the
same periodic address for positive and negative coordinates while removing
two runtime integer remainders per lookup. Other dimensions retain the
original expression, and clamp addressing retains its original result.

Height resolution, bilinear samples, traversal order, cell budgets, root
solving, hit UVs, normals and depth calculations are preserved. The measured
fetch volume remains approximately 231/232 KB per expensive depth/forward
draw. The gain comes from less address computation, with no sampling reduction.

## Same-frame A/B/A replay

The optimized pixel bytecode was extracted from a new **running-engine capture**
after O3DE applied its material specialization constants. RenderDoc replaced
only the two pixel shaders in the original capture, measured the frame, then
removed those replacements and measured again. Geometry, resources, camera,
material values and all other shaders stayed in that captured state.

AMD vendor-counter `GPUTime`, milliseconds, for original events 989 and 2599:

| Phase | Depth draw | Forward draw | Combined disjoint draw time |
|---|---:|---:|---:|
| Original A1 | 0.83088 | 0.81808 | 1.64896 |
| Optimized B | 0.48916 | 0.47308 | 0.96224 |
| Restored original A2 | 0.84308 | 0.82304 | 1.66612 |

The reduction is 41.6% versus A1 and 42.2% versus A2. Pixel-shader busy time
falls from 0.765/0.753 ms to 0.449/0.434 ms. These are serialized replay draw
diagnostics, not native frame times; stage busy times can overlap.

Both B and A2 match A1 byte-for-byte for the 1920 × 1080 RGBA output and the
1920 × 1080 linear-depth attachment. Depth is read immediately after its
producing draw, event 1061, while its transient storage is valid.

- Color SHA-256: `0f37c4dd7bd4fb1b9a62243977889f44ed2fc2032e27390ddaa4dd48b41e9132`
- Linear-depth SHA-256: `0f5024f9526929d92af36da6fd0bd281424b971eb209415c90e311dd1ab6423b`

Two exploratory checks are explicitly excluded: raw asset bytecode without
runtime specialization, and a depth read after its transient storage had been
reused. Their logs remain in the evidence directory. The final replay requires
matching runtime vertex bytecode and identical valid color/depth outputs.

## Native measurements

RX 7900 XTX, driver 32.0.31041.1004, Ryzen 7 2700, Windows profile Editor/DX12,
1920 × 1080, render scale 1, 2× MSAA, VSync off and `sys_MaxFPS=-1`.
The default terrain material, saved lighting and terrain settings match #17's
baseline. Native runs have no RenderDoc injection.

Each main case requests 600 GPU-frame samples after an eight-second settle,
with nine separate pass-timestamp captures. Original attribution cases request
240 frames. GPU frame time is Atom's timestamp envelope, never a sum of passes.
Three far-view repetitions appear at the start, middle and end of each full run.

| Stationary far, three repetitions | Original A2 | Optimized B |
|---|---:|---:|
| GPU frame medians | 4.103–4.127 ms | 3.551–3.567 ms |
| GPU frame p95s | 4.421–4.454 ms | 3.952–4.002 ms |
| Depth-pass medians, whole scene | 1.036–1.083 ms | 0.792–0.911 ms |
| Forward-pass medians, whole scene | 1.230–1.238 ms | 0.993–1.082 ms |
| Sampled graphics-clock medians | 2545–2563 MHz | 2297–2359 MHz |

The earlier A1 attribution run's two unchanged far-view medians were
4.123/4.114 ms, agreeing with the restored-original run. The optimized far
improvement remains despite its lower observed graphics clocks.

Other native GPU-frame medians, retained to avoid implying a uniform gain:

| View | Original A2 | Optimized B |
|---|---:|---:|
| Near | 3.146 ms | 3.332 ms |
| Grazing | 3.319 ms | 3.593 ms |
| Near, moving | 3.577 ms | 3.734 ms |
| Grazing, moving | 3.415 ms | 3.747 ms |
| Far, moving | 3.631 ms | 3.961 ms |

GPU clocks were **sampled, not locked**. For example, grazing graphics-clock
medians were 2378 MHz original versus 1946.5 MHz optimized, and far-moving
medians were 2365 versus 1886 MHz. These runs cannot separate the shader change
from frequency and scheduling effects in whole-frame comparisons across all
views. They satisfy the numerical stationary-far and isolated-draw targets;
they are not a fixed-clock certification or an Editor FPS claim.

Read-only AMD PMLog telemetry uses the official
[AMD Display Library sensor definitions](https://github.com/GPUOpen-LibrariesAndSDKs/display-library/blob/master/include/adl_defines.h).
The older OverdriveN status API returned unsupported on this driver. Approximate
GPU-collection windows are reconstructed from case start and raw-file write
timestamps; full half-second telemetry is preserved. No GPU or OS settings
were changed to force clocks.

## Correctness and reproduction

All six standalone suites pass: core intersection, planar faces, face normals,
blended artifacts, relief shadows and hardware D3D12 conformance. The core suite
performs **1,204,537 checks**, including 20,000 rays against an independent
double-precision oracle. Added address checks cover repeat/clamp, power-of-two
and other dimensions, negative indices, seams and signed integer extremes.
The D3D12 suite verifies 8192 rays.

Close-up front and grazing screenshots are pixel-identical outside the live
performance overlay, including the wall silhouette. The near scene screenshot
also matches. Some other scene screenshots differ in 32 pixels at Editor icons;
the exact same-frame replay independently verifies complete color and depth.
Timed moving routes are short +X motion at 16 m/s; their screenshots are the
starting poses, not continuous temporal-quality tests.

The close-up wall cameras are `(5, -14.015765, 18.0022)` / `(0, 0, 90)` and
`(0.15, -18, 18.0022)` / `(0, 0, 2.15)` (position / Euler degrees), with terrain
temporarily disabled. Main cameras are near `(16,16,32)`, far `(16,-180,180)`
and grazing `(16,-100,32)`, rotations `(-35,0,0)`, `(-35,0,0)` and `(-5,0,0)`.

[Compact measurements](MeshPixelPerformanceData.json) accompany this report.
Raw evidence, configurations, original/optimized shader snapshots, telemetry,
replay scripts, captures and images are in
`D:/TG/TGProject/user/MeshPixelProfiling/`. `native-b1` is optimized;
`attribution-01` and `native-a2` are original. `shader-aba-runtime.json` is the
final same-frame validation. The profiling driver is terrain-compositor's
`tools/ProfileTerrainGpu.py`; each run uses its corresponding saved JSON in
`TC_GPU_CONFIG`. Asset changes require a completed AssetProcessorBatch run
before launching a measurement session. Replay uses RenderDoc 1.25's AMD
counters and the retained `ExtractRuntimeShaders.py` / `ReplayShaderABA.py`.

```powershell
cmake --build D:/TG/TGProject/Gems/silpom/build/planar-core --config Release
ctest --test-dir D:/TG/TGProject/Gems/silpom/build/planar-core -C Release --output-on-failure
```

The optimized source and compiled project assets are left applied. Shader asset
builds report zero failed assets and zero errors; existing locale warnings and
Editor shutdown pipeline assertions remain in logs. The saved level SHA-256
stays `83d74525d32b75bdfbcab08cc7df7379ca06dc82d2a2185b755440bcd32a8728`.

# Ryzen 5700G rebenchmark of main 2f818d8

Measured on 2026-11-14 after updating to upstream
`2f818d87cd29663fab360cc7ccde06e117c257cf`. The new **2x project default** passes
the tower visual checks. Its largest measured pass is upper-glancing hit
generation: **182.09 ms median / 187.17 ms p95**. A separate matched 1x run
keeps the comparison with the original `e968424` baseline meaningful.
No shader optimization was made.

The local branch is `codex/benchmark-main-2f818d8`, built at merge commit
`e39e59db82ec0cc3570700281b0515f970492678`. The Assets tree exactly matches
upstream main. The only retained native difference is the earlier
`PlanarBenchmark` CPU-frame-period telemetry. This follow-up changes the
profiling Python harness to accept 1/2 samples and renderer-wide AA, and to
read exact 2x integer hit/depth records through the existing inspection atlas.
Main remains at the upstream tip; nothing was pushed or merged back.

## Configuration and validation

Actual rendering GPU: **AMD Radeon(TM) Graphics**, PCI **1002:1638** (Cezanne
integrated), confirmed by each Editor startup and every timestamp capture.
Driver **31.0.21923.11000**, dated 2025-07-01 UTC. CPU: Ryzen 7 5700G,
8 cores/16 threads. RAM: **32 GiB, 2 x 16 GiB G.Skill F4-3200C16-16GVK**, slots
A1/B1, configured at **2133 MT/s**. DXGI's earlier same-driver probe reports
496 MiB dedicated and 15.7 GiB shared GPU memory. Windows 11 Pro 10.0.26300;
Balanced power plan unchanged. Results apply to this integrated-GPU and RAM
configuration; these timestamps do not isolate bandwidth or instruction stalls.

The source build uses **DX12, SM 6.2, 1280 x 720**, with explicit source engine
`D:/engines/o3de-tg-profile`; the installed SDK remains intact. Startup prints
`TG DX12 shader model 6.2; advanced texture operations: supported.` Compiler
configuration remains CMake 4.3.1, VS 2026 / MSVC 19.51.36260.0,
Windows SDK 10.0.26100, four build jobs.

AA is **renderer-wide Current default**, `r_silpomAntiAliasing=0`, SMAA off.
The primary run reports `[2,0,0]` in Editor and Ctrl-G. The separate 1x
comparison uses a process-start `r_multiSampleCount=1` override and reports
`[1,0,0]`; it is not the new project's default sample count.

Both `TowerBenchmarkEditorSmoke.py` and `TowerBenchmarkVisualTests.py` pass at
both sample counts. All four Editor views and Ctrl-G screenshots were reviewed.
The tower renders, the interior column is illuminated, and there are no newly
black wall panels. Fine grazing contour aliasing/cutouts remain visible and
also occur in the historical 1x reference. These are not evidence of a new
2x backup-depth mismatch: exact per-sample comparisons find **zero mismatches**
across 17 lower-facing, 34,372 lower-glancing and 58,948 upper-glancing backup
samples. This check does not claim every relief edge is artifact-free.

Height-derived normals/sampling remain **on**, with gather validation and R16
heights on, estimator 5 and the saved tower/lighting unchanged. As in the
original measured baseline, the harness enables `UseHeightDerivedNormals=true`
at runtime because the frozen serialized fixture contains false; it never saves
the level. Effective configuration SHA-256 remains
`b76f275b9af10b14731ac7dc5115e1e29eb6edb11cebd57cd05e22b3b32ec67e`.

No device-loss, renderer binding or assertion messages occur in the four run
logs. Ctrl-G still reports the unrelated missing Yarn `BuildCredits.ps1` script;
tower rendering succeeds. TG73 lacks `Editor/Scripts/rendering_controls.py`, so
the separate settings-pane smoke was not run; native AA cvar/state was checked.
SM 6.7, 4x MSAA and RenderDoc counters were not measured in this follow-up.
Developer Mode remained off and RenderDoc was not injected.

## Timing results

Each row pools **three independent 120-frame captures**, 360 accepted frames.
Camera poses come from `Levels/SilPOMTowerBenchmark/scene.json`. Warm-up is
90 frames after camera selection, then 120 + 30 frames before each capture;
the native harness discards another 16 warm-up query samples. Asset Processor
was idle. Timing directories contain no PNG/DDS captures, and their timestamp
pass lists contain no inspection passes. All accepted frames are retained.

Values are **median / p95, milliseconds**. Hit generation here means the
`SilPOMHitDepth` leaf; classify/fill/backup costs remain separate leaf passes.
Graphics span covers the first through last graphics-pass timestamps and can
contain queue gaps. It is **not GPU busy time or FPS**. CPU frame period is
the RHI frame-to-frame period, including waits, sampled when delayed GPU
queries arrive; it is neither CPU busy time nor paired CPU work for that GPU
frame. Parent pass durations are not added to children.

### Current default, 2 samples

| View | Hit generation | SILPOM forward | Graphics span | CPU frame period |
| --- | ---: | ---: | ---: | ---: |
| Exterior | 19.28 / 19.45 | 25.23 / 30.64 | 64.61 / 70.11 | 64.95 / 70.46 |
| Lower-facing | 79.70 / 80.18 | 100.13 / 104.16 | 200.31 / 205.16 | 200.94 / 205.96 |
| Lower-glancing | 125.44 / 125.73 | 105.38 / 108.95 | 251.95 / 257.52 | 252.63 / 258.27 |
| Upper-glancing | 182.09 / 187.17 | 90.43 / 92.17 | 296.14 / 301.36 | 296.90 / 302.06 |

### Matched 1-sample comparison

| View | Hit generation | SILPOM forward | Graphics span | CPU frame period |
| --- | ---: | ---: | ---: | ---: |
| Exterior | 10.95 / 11.06 | 11.93 / 14.18 | 38.18 / 40.42 | 38.53 / 40.74 |
| Lower-facing | 47.68 / 48.19 | 49.80 / 51.26 | 112.12 / 114.74 | 112.49 / 115.24 |
| Lower-glancing | 75.55 / 76.61 | 52.29 / 53.84 | 143.86 / 146.74 | 144.28 / 147.20 |
| Upper-glancing | 114.10 / 115.71 | 45.03 / 45.75 | 175.15 / 178.07 | 175.75 / 178.72 |

Spread between the three captures' **graphics-span medians**, milliseconds:

| Samples | Exterior | Lower-facing | Lower-glancing | Upper-glancing |
| --- | ---: | ---: | ---: | ---: |
| 2 | 64.575–64.637 | 200.159–200.416 | 251.418–252.271 | 296.005–296.226 |
| 1 | 38.113–38.243 | 112.102–112.146 | 143.844–143.900 | 174.971–175.324 |

The compact JSON records each capture, every metric's repeat-median range and
pooled min/max/p95. Upper-glancing 2x hit-generation repeat medians span
182.059–183.523 ms.

Hit generation remains the largest cost in glancing views. Forward shading is
largest in exterior/lower-facing. At 2x, forward medians are approximately
twice those of the new 1x run; graphics-span medians are 1.69–1.79 times 1x.
Other notable 2x leaf medians are ordinary Forward (4.13 ms exterior), wall-hole
classification (1.05–2.02 ms), fullscreen shadows (1.09–1.21 ms), and Editor
outline feedback (0.86–0.87 ms). This is elapsed pass profiling, not a
source-line, occupancy or cache-stall ranking.

Against the earlier `e968424` 1x baseline, hit-generation medians change by
-1.03% to +0.16%. Interior graphics spans change by +0.57% to +1.77%, and
forward medians by +1.13% to +2.30%. Exterior span is 4.99% lower, but its
forward repeat medians (11.87–11.97 ms) fall within the old 11.74–13.95 ms
range. This does not establish an exterior optimization benefit. The old and
new default-AA results have different sample counts and must be labeled so.

## Provenance and local evidence

Diversion remains clean at `dv.commit.73`. TG's SILPOM pin remains
`e9684244b85e878b445606164b157b16438398d4`; manifest SHA-256 is
`627fc62526d0dc10f738dcdc7a1ceb7c8f1973feb87f6723f4807bf28a5c2fdc`.
Scene SHA-256 remains
`a0894326d0eb68b7328949b1f1829a8021b7293ce16a567e0c2ea8c9148f6e7b`.
Earlier profiling branches, reports, captures and preserved local work remain.

Bundle **tg-o3de-20261114**: O3DE base
`061180bf24f1666eb30315b35da292eb14f4659c`, terrain base
`ee6887b4b743047f7dece1b179c0cf30cb281880`. LF-normalized patch SHA-256:

- Engine: `3dd9ed2e11485f7592f7462e7da850b154ca30c1159994b1ca6f0fcfec7877f3`.
- Terrain: `dab37b902de3e3ca417531b6b08d9fed59c20dc02382c12426b3500a89137841`.

All native build targets succeeded. AssetProcessorBatch processed 627 assets,
zero failed. Three diagnostic cache products predated the checkout timestamp;
their old files were preserved and regenerated in a second asset pass
(540 processed, zero failed). Final readiness has no stale products or failed
jobs. `apply.py --verify-only` matches all 95 approved integration file hashes.
AssetBuilder's disconnect error occurs during Asset Processor shutdown, after
quit was requested; no asset job failed. Build logs retain that context.

Local raw evidence is under
`D:/diversion/TG/TGProject/user/silpom-rebenchmark-2f818d8-20261114/`:

- `visuals-2x-20261114-205859`, `visuals-1x-20261114-210245`: PNGs, DDS hit/depth readbacks, logs and visual validation.
- `timings-2x-20261114-210459`, `timings-1x-20261114-211916`: 24 raw timestamp JSONs, logs and complete pooled analyses.
- `build.log`, `build-plan.log`, `engine-integration-build.log/json`, final bundle/readiness checks, hardware inventory and evidence hash manifest.

The build directory also retains its official `engine-integration-build.log`
and `.json`. CMake synchronized `VueGroundSampling.azsli` from terrain during
configuration; the generated copy was preserved locally and the published
DV73 source restored before asset processing. Do not repin dependencies or
stack legacy component patches when reproducing this already-patched checkout.

Compact statistics, raw timing hashes, harness hashes and full provenance:
[Ryzen5700GMain2f818d8-20261114.json](../Tests/Reference/Ryzen5700GMain2f818d8-20261114.json).

## Reproduce on this checkout

Preserve local work and follow TG's `EngineIntegration/O3DE/README.md` for
first-time dependency restoration and bundle application. The build used:

```powershell
$project = 'D:/diversion/TG/TGProject'
$engine = 'D:/engines/o3de-tg-profile'
$python = 'C:/Users/david/AppData/Local/Programs/Python/Python312/python.exe'
powershell -NoProfile -ExecutionPolicy Bypass -File "$project/EngineIntegration/O3DE/Build-ProfilingEngine.ps1" -EnginePath $engine -ProjectPath $project -ShaderModel 6_2 -Parallel 4 -Generator 'Visual Studio 18 2026' -Python $python
& $python "$project/EngineIntegration/O3DE/apply.py" --engine $engine --project $project --verify-only
```

Check the plan first, close Editor/asset tools, preserve the published TG source
if CMake synchronizes it, and finish asset processing/readiness before capture.
Run the following for **visuals at 2 then 1 samples**. Review all images/logs
before running it for **timings at 2 then 1 samples**. Each invocation creates
a fresh ignored output directory. Runtime height-derived sampling is asserted
on by the fixture. No level is saved.

```powershell
$samples = 2                 # Repeat separately with 1
$kind = 'visuals'            # Use 'timings' only after visual validation
$build = "$project/build/profile-5700g-sm6_2"
$output = "$project/user/tower-$kind-${samples}x-$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory -Path $output | Out-Null
$env:SILPOM_ESTIMATOR_OUTPUT = $output
$env:SILPOM_TOWER_SAMPLES = "$samples"
$env:SILPOM_MSAA_TIMING = if ($kind -eq 'timings') { '1' } else { '0' }
$env:SILPOM_TOWER_TIMING_ONLY = $env:SILPOM_MSAA_TIMING
$env:SILPOM_TOWER_TIMING_REPEATS = '3'
$env:SILPOM_TOWER_RENDERDOC_VIEW = ''
$arguments = @("--project-path=$project", "--engine-path=$engine",
    "--project-build-path=$build", '--rhi=dx12',
    "--regset=/O3DE/Autoexec/ConsoleCommands/r_multiSampleCount=$samples",
    '--runpython', "$project/Gems/silpom/Tests/TowerBenchmarkEditorSmoke.py")
$process = Start-Process "$build/bin/profile/Editor.exe" -ArgumentList $arguments -WorkingDirectory $project -WindowStyle Hidden -RedirectStandardOutput "$output/editor.stdout.log" -RedirectStandardError "$output/editor.stderr.log" -PassThru
$process.WaitForExit()
if ($process.ExitCode -ne 0) { throw 'Editor failed; inspect logs' }
Copy-Item "$project/user/log/Editor.log" "$output/Editor.log"
if ($kind -eq 'visuals') {
    & $python "$project/Gems/silpom/Tests/TowerBenchmarkVisualTests.py" $output "$output/visual-validation.json"
} else {
    & $python "$project/Gems/silpom/Tests/analyze_tower_profile.py" $output --output "$output/summary.json"
}
if ($LASTEXITCODE -ne 0) { throw 'Validation failed' }
```

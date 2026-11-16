# Frozen TG O3DE integration

This is the complete source delta of the engine used for the approved SILPOM
tower reference on 2026-11-14. It is not an installed SDK update. Apply it to
the pinned O3DE source revision and rebuild native modules and processed assets.

- Upstream: <https://github.com/o3de/o3de.git>
- O3DE base: `061180bf24f1666eb30315b35da292eb14f4659c` (2.7.0).
- SILPOM visual/profiling reference: `e9684244b85e878b445606164b157b16438398d4`.
- Terrain integration base: `ee6887b4b743047f7dece1b179c0cf30cb281880`.
- Bundle identifier: `tg-o3de-20261115`. Exact input/output hashes and patch
  hashes are in `manifest.json`. Text hashes normalize CRLF to LF.

`engine.patch` covers 91 source/configuration files, including seven added
files, relative to the upstream Git commit, not a modified working checkout.
It includes DX12 SM 6.2/6.7 selection and builder fingerprints; MSAA attachment,
pass and probe-grid fixes; decal painting APIs and compositing shaders; tower
light controls; prefab/asset-import and animation/skinning fixes; PhysX picking,
character push/support changes; MiniAudio changes; and the existing TG window,
sky-module and engine configuration changes. These older changes are retained
because TG and its pinned gems were built against this engine.

`terrain-compositor.patch` covers four integration files: the MSAA pass-tree
lifecycle correction, shader conversion/gradient validation fixes and explicit
shader-preprocessor encoding settings. It intentionally leaves the separate
VueLiveLink authoring/export changes out of this profiling integration.

The files under the older `scripts/EnginePatches` and SILPOM's
`Docs/Integration/*Msaa.patch` are historical component patches. Do not stack
them on top of this aggregate. The aggregate already contains their applicable
final changes. Existing fully matching files are accepted; a conflicting or
partially modified file is rejected before any component is applied.

The 2026-11-15 revision adds shared-buffer spatial paint groups and exclusive
cell clipping to `DecalComposite.azsli`. Use it with the spatial-grouping painter;
the single-volume buffer format remains compatible. Stamps affect color/normals,
not the SiLPOM heightmap or its relief bounds.

## Preparing the profiling PC

Install the C++ build prerequisites for this O3DE revision, CMake, Git with Git
LFS, and Python 3.9 or newer. An existing SDK can remain installed. Use a
separate source checkout and build directory; do not copy this PC's DLLs,
CMake cache, shader cache, or absolute engine paths to the profiling PC.

Pull Diversion **main**, restore `TGProject/scripts/GitDependencies.json`, and
keep the SILPOM pin at `e968424`. Restore dependencies before applying this
bundle: the terrain checkout will intentionally become modified afterwards.
The bundle is mirrored in SILPOM GitHub main at `Integration/O3DE`, independently
of the frozen SILPOM implementation pin. `HANDOFF.md` is the complete agent prompt.

In PowerShell, choose a new engine directory and the actual TG checkout:

```powershell
$engineSource = 'D:/engines/o3de-tg-profile'
$tgProject = 'D:/TG-assets-gameplay/TGProject'
git lfs version
git clone --no-checkout https://github.com/o3de/o3de.git $engineSource
git -C $engineSource lfs install --local
git -C $engineSource checkout --detach 061180bf24f1666eb30315b35da292eb14f4659c
git -C $engineSource lfs pull

python "$tgProject/EngineIntegration/O3DE/apply.py" --engine $engineSource --project $tgProject --check-only
powershell -NoProfile -ExecutionPolicy Bypass -File "$tgProject/EngineIntegration/O3DE/Build-ProfilingEngine.ps1" -EnginePath $engineSource -ProjectPath $tgProject -ShaderModel 6_2 -PlanOnly
powershell -NoProfile -ExecutionPolicy Bypass -File "$tgProject/EngineIntegration/O3DE/Build-ProfilingEngine.ps1" -EnginePath $engineSource -ProjectPath $tgProject -ShaderModel 6_2 -Parallel 4
```

Check each command's exit status and stop on failure. Save and close Editor and
asset tools before the real build. The script refuses to build while those
processes are running. `-Python` accepts an explicit Python executable if the
Windows `python` alias does not resolve to a working installation. Select an
installed Visual Studio generator with `-Generator` if CMake cannot select the
desired compiler; for example `-Generator 'Visual Studio 17 2022'`.

The build script preflights both patches, applies them, bootstraps engine
Python, preserves other keys while selecting the source engine in
`TGProject/user/project.json`, and configures a separate
`build/profile-5700g-sm6_2` directory. It builds Editor, asset tools, shader
builders, the DX12 RHI, SILPOM runtime/editor modules and the TG game launcher,
then runs AssetProcessorBatch for PC assets. Engine/gem dependencies are rebuilt
by CMake. No full clean rebuild is forced. It records the source revisions,
patch identity, shader target and build paths in `engine-integration-build.json`
and command output in `engine-integration-build.log` in that build directory.

SM 6.2 is the profiling script's compatibility default. The approved reference
was captured with DX12 SM 6.7 on the original PC. Actual GPU/driver support must
be checked on the Ryzen PC; the CPU model does not identify the rendering GPU.
The current full tower has not yet been validated end to end on that PC with
SM 6.2. Validate the rebuilt scene before collecting timings. There is no
automatic runtime shader-model fallback. If the GPU supports SM 6.7, that target
can be selected explicitly, with its own matching native build and shader assets.
This script does not change the saved AA, estimator, lighting or tower settings.

Launch the newly built Editor, not the SDK's Editor:

```powershell
& "$tgProject/build/profile-5700g-sm6_2/bin/profile/Editor.exe" "--project-path=$tgProject" "--engine-path=$engineSource"
```

Allow asset processing to finish and consult the Editor/Asset Processor logs.
Open `Levels/SilPOMTowerBenchmark/SilPOMTowerBenchmark.prefab`. Verify the four
named views and Ctrl-G with visible lighting and intact stone edges. Follow
`Gems/silpom/Docs/TowerBenchmark.md` for capture and timing procedures. Record
the actual GPU/driver, resolution, shader target, AA, camera and component
settings. Start with the unchanged default AA at 1280 x 720; capture images and
GPU timings in separate runs. No new optimization is authorized by this handoff.

## Verification

```powershell
python "$tgProject/EngineIntegration/O3DE/apply.py" --engine $engineSource --project $tgProject --verify-only
```

`--check-only` makes no source changes. Normal application checks both source
repositories' exact HEAD revisions and all touched-file hashes before changing
either repository. It never resets, cleans, checks out, commits or pushes a
repository. It leaves unrelated files alone. Reapplying is a no-op after all
output hashes match. Exact installed decal shaders from `cd425e4`
are upgraded using the separately hashed modification patches in `migrations/`.
An edited predecessor is rejected and preserved; unknown revisions are not guessed.
The exact `f9e3d7c`, `dada01d` stable and `dada01d` experimental shaders
are upgraded through separately hashed migrations. Switching between the updated
stable and experimental renderers retains the same exact-hash protections.
The current target restricts surface-addressed dabs to their owning receiver,
allowing the brush footprint to cross its canonical triangle edges. UV prefixes
retain their explicit chart-face restriction. Receiver rejection also runs in
the reversed alpha path, preserving opacity early-out, and both native traversal
paths honor the material hook for claiming a fixed prefix. Ordinary materials do
not compile the prefix-claim hook. The receiver-UV proof and its validation contract
are documented in `../../Docs/ReceiverUvPaint.md`; its bake is explicitly selected
on a bound receiver and does not create a world-space cache.

The rejected color/normal bake prototype is isolated under `experiments/`.
Only `apply.py --experimental-paint-bake` installs it; verification needs the same
flag. The default installer restores an exact, unedited experimental shader to
stable. It refuses edited experiments. This prototype was slower in the measured
cases and its accepted normal samples failed quality checks. It is disabled in
the native painter and excluded from the default engine patch. See
`../../Docs/DecalPaintBakeBenchmark.md` for measurements. Installing or restoring
it requires a full dependent shader/material asset rebuild.
`--verify-only` also supports the original source copy
without Git metadata; it requires every patched file to match.

`validate_bundle.py --baseline <existing-O3DE-checkout> --terrain <existing-terrain-checkout>
--output <new-scratch-directory>` exercises clean application, CRLF handling,
idempotence, conflicting-file refusal and the PowerShell command plan using
isolated sparse source clones. It does not rebuild O3DE. `VALIDATION.json`
records the packaging checks; it is not a claim of a completed Ryzen build.

Original O3DE copyright and Apache-2.0 OR MIT license notices are retained in the
patches. The upstream engine checkout supplies its complete license files.

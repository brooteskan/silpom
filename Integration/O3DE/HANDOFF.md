# Ryzen 7 5700G profiling handoff

Continue the SILPOM tower profiling setup on this PC. Read this entire prompt
and `EngineIntegration/O3DE/README.md` before building. The installed stock O3DE
SDK is not sufficient: current SILPOM calls custom engine decal APIs, and the
approved setup includes native renderer, lighting and import fixes.

1. Inspect local work before updating anything. Preserve existing changes; do
   not reset or force checkout a dirty repository. Pull the newest Diversion
   **main** containing `TGProject/EngineIntegration/O3DE/manifest.json`, bundle
   `tg-o3de-20261114`, and the portable lit tower scene. The older scene-only
   commit `dv.commit.66` is insufficient. Restore the project Git dependencies
   using `TGProject/scripts/Restore-GitDependencies.ps1` before applying patches.
   Keep TG's SILPOM pin at
   `e9684244b85e878b445606164b157b16438398d4`.
2. In the SILPOM checkout, fetch origin and create
   `codex/profile-ryzen-5700g` from that exact `e968424` commit. Verify the
   starting SHA. If that branch already exists, inspect it and preserve its
   work instead of recreating or overwriting it. GitHub SILPOM main contains a
   mirror of this bundle under `Integration/O3DE`; the frozen implementation
   branch does not need a cherry-pick of the packaging commit because Diversion
   already supplies the bundle outside the gem.
3. Identify the actual rendering GPU and driver, installed compiler/CMake,
   available RAM and the existing SDK. Keep the SDK intact. Clone O3DE source
   into a separate directory and check out
   `061180bf24f1666eb30315b35da292eb14f4659c`; fetch its LFS content. Use the
   exact commands in README, substituting this PC's paths. The terrain gem must
   start at `ee6887b4b743047f7dece1b179c0cf30cb281880`.
4. Run `apply.py --check-only` and `Build-ProfilingEngine.ps1 -PlanOnly` first.
   Inspect the result. Save and close Editor/asset tools, then run the build
   script with explicit `-EnginePath`, `-ProjectPath`, `-ShaderModel 6_2` and a
   suitable `-Parallel` value. SM 6.2 is the compatibility starting point,
   subject to the actual GPU/driver. The original reference used SM 6.7; do not
   assume the 5700G's CPU model guarantees that GPU feature level. Use SM 6.7
   only after verifying support, with matching rebuilt engine and shader assets.
   Never apply legacy component patches on top of the aggregate bundle.
5. Confirm native build and AssetProcessorBatch succeeded. Run
   `apply.py --verify-only`. Retain the build log and
   `engine-integration-build.json`. Launch Editor from the new build directory
   with explicit project and source-engine paths. Inspect startup logs for the
   actual GPU, shader model and errors; do not silently launch the old SDK.
6. Open `Levels/SilPOMTowerBenchmark/SilPOMTowerBenchmark.prefab`. Preserve its
   saved tower, estimators, lighting and **Current default** AA. Verify exterior,
   lower facing, lower glancing, upper glancing and Ctrl-G. The scene has sun,
   environment lighting, window lights and two interior fills. Black walls,
   missing surfaces, reappearing edge breakup or validation errors are failures
   to investigate before timings. The complete SM 6.2 tower run is not yet
   validated on this PC; report any compatibility issue explicitly.
7. Establish a baseline at 1280 x 720. Finish asset processing and warm up
   shaders first. Use the existing tower benchmark capture/timestamp scripts;
   separate captures from timings. Run three 120-frame GPU timing captures per
   camera and report frame/pass medians and spread, actual GPU/driver, CPU,
   resolution, shader target, AA, scene/gem revisions and bundle patch hashes.
   A one-second FPS average alone is not enough to identify GPU bottlenecks.
   Compare 4x MSAA only after the unchanged default passes visual validation.

Do not change the approved nearest-hit recovery, estimator default, AA default
or tower configuration. The user stopped further optimization; this handoff
authorizes setup, compatibility corrections required to run the reference and
profiling. Report measured bottlenecks and visual results before proposing new
optimization work. Do not merge to SILPOM main, repin TG or publish further
changes without the user's instruction.

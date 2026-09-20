# Imported mesh integration — experimental

This is the in-progress implementation of issue #2, not an acceptance or
production-readiness claim. The existing planar Patch path remains separate.

The former inline pixel solver has been replaced with two compute stages and
thin raster consumers. Raster activation still defaults to off; **Enable
experimental raster** is a diagnostic opt-in, not a production-readiness claim.
The small imported fixture now renders in the DX12 Editor and passes the
hit/depth/albedo consumer checks below. This does not complete issue #2.

## Resource path

The `SilPOM.Builder` module recognizes `*.silpom.json`, checks the paired FBX
hash, imports the FBX through SceneAPI, validates the version-2 transport against
that graph, and emits a versioned `.fbx.silpommesh` product. Logical vertex/face,
object and material identities, corner attributes, tags and adjacency survive
in that product. The builder does not edit the FBX manifest. FBX and manifest
changes invalidate the sidecar's build job.

One **SilPOM Mesh** component owns the ordinary and displaced batches. It
discovers authored material identities and profile IDs from the surface asset.
Assign the source materials and a normalized linear, single-mip scalar height
image for each profile. Profile controls expose world-metre amplitude, reference
height, UV tiling/offset and repeat/clamp addressing. An internal material variant
is managed by the component; do not apply it manually to stock base triangles.

Only ordinary faces enter the component's ordinary draw batches. Transport UVs
are not included in those buffers. Tagged faces use conservative projected
bounding-box coverage and the shared curved intersection kernel. Grouping is by
authored material and displacement profile, not by triangle. The stock FBX
builder may still emit an independent, unfiltered diagnostic `.azmodel`; do not
attach that model alongside the SilPOM Mesh component.

The component stages models, material snapshots and immutable traversal buffers
with draw items disabled before switching all owned batches together. Atom skips
initialization of invisible handles, so the packet-update hook disables drawing
before culling instead. Failed preparation retains
the previous generation and reports an error through `SilPomMeshRequestBus`;
`IsReady` means readiness for the current requested configuration, not merely
that an older generation is still visible.

## Reproduce the small integration fixture

From the Gem directory, using an empty output directory inside a project's asset
scan folder:

```powershell
& '<blender.exe>' --background --factory-startup --python-exit-code 1 --python Tests/create_mesh_fixture.py -- --output D:/TG/TGProject/Assets/SilPOMMeshIntegration
cmake --build D:/TG/TGProject/build/windows --config profile --target SilPOM.Builder SilPOM.Editor SilPOM
& D:/TG/TGProject/build/windows/bin/profile/AssetProcessorBatch.exe --project-path=D:/TG/TGProject --platforms=pc --regset-file=D:/TG/TGProject/Gems/silpom/Tests/mesh_dx12_test.setreg
```

Do not rebuild loaded DLLs while Asset Processor or Editor is running. On Windows,
the account running Asset Processor needs read access to the exported bundle.
The fixture generator installs the real add-on in a disposable Blender script
directory, tags faces through its operators and exports through its normal
export operator. It does not change the user's saved Blender preferences.

Use the temporary DX12 platform-tag override for this experiment. It does not
change saved project configuration. The former inline shader failed Vulkan
compilation; the redesigned compute path has not been validated on Vulkan.

The fixture has four noncoplanar triangles, two source materials, three tagged
faces and one ordinary face sharing a material with tagged geometry. Its 2x2
height image intentionally bounds the initial traversal workload. The exported
authoring copy and `fixture.json` record the exact generated identities.

The optional `Tests/mesh_editor_smoke.py` drives the same component properties in
an unsaved TG DefaultLevel, beginning at a 160x120 viewport, single-sample raster
(`r_multiSampleCount 1`) and a single 256-pixel shadow cascade. Launch Asset
Processor with the same temporary settings, then a
separate Editor with `--rhi=dx12 --project-path=D:/TG/TGProject --runpython <absolute-script-path>`.
It records status, screenshots, GPU timestamps, camera/shadow depth, forward
albedo and raw hit-buffer readbacks under
`user/SilPOMMeshStagedValidation` (the earlier blocked run is preserved separately).
Set `SILPOM_MESH_OUTPUT` to a different output directory to preserve a run.
Before the static captures, a TickBus callback translates and rotates the camera
continuously through 12 asynchronous forward-albedo/hit-buffer capture pairs.
The callback stays connected until both readbacks complete. The test rejects
view errors, Invalid rays, missing fixture colors and any magenta pixel not
explained by an explicit Exhausted record at that pixel in the same frame.
Actual bounded exhaustion remains visible and is counted, not hidden or treated
as a certified hit. This tests active-motion synchronization, not just settling.
The motion hit-buffer artifacts add approximately 576 MiB per run.
The readback checks finite certified hits, unit normals and the requested error
bound, and explicitly rejects Exhausted/Invalid samples. Depth and albedo checks
verify that the raster consumers use those records; they are **not** an
independent surface/root oracle. Recheck the captured files without Editor using
`python Tests/mesh_capture_analysis.py <project>/user/SilPOMMeshStagedValidation`.
This is test automation, not a replacement for usable artist controls or game
validation. The test restores its temporary editor preferences and never saves
the level.

## Staged compute architecture

`MeshRenderFeatureProcessor` inserts `SilPomIntersect` and `SilPomResolve` before
each pipeline's rendering work. The first stage emits the complete bounded
intersection result, including unresolved intervals and exhaustion reasons. The
second runs the shared exact/rational continuation and writes final position,
normal, authored UV, tangent frame and projected depth records. Neither solver is included in
the material shader. Miss/Hit/Exhausted/Invalid states remain distinct.

Each material/profile batch has a view directory keyed by the actual jittered
view-projection matrix, viewport dimensions and near-depth convention. Camera
depth and color share one result; each directional-light cascade has its own.
The frame's view snapshot is taken in `SetupFrameGraphDependencies`, after
`Scene::UpdateSrgs` refreshes the jittered matrices used by the raster ViewSrg.
Taking it in pass `FrameBegin` uses stale cached matrices during camera movement.
Intersect prepares one snapshot that Resolve reuses; the matrix-match diagnostic
remains strict rather than hiding mismatches by widening its tolerance.
The projected depth bits are consumed verbatim rather than recomputed by
separately optimized material shaders.
Compute traces only the conservatively projected rectangle, reconstructing
finite camera/light rays and clipping them to the batch bounds. A near-plane
ambiguity conservatively covers the entire viewport. Material UVs are restored
from height-profile tiling/offset, and tangents use the full surface derivative.

The hit buffer is an explicit read/write frame-graph attachment in both compute
stages and an explicit fragment-read attachment on consuming raster passes.
This supplies UAV ordering and read transitions; bindless lookup is only the
shader's resource-addressing mechanism. Frame scopes also declare traversal
buffer reads. Ordinary geometry continues through the existing mesh processor.

The first implementation deliberately bounds resource use: eight live displaced
batches per scene (including pending generations), 32 distinct views per
pipeline, and 65,536 projected samples per batch across those views. A shared
48.03 MiB hit/scratch allocation is created lazily. Overflow is an explicit
Exhausted diagnostic, not clipping or a false Miss. Unsupported views are
Invalid. New generations are not published until their stages and packets are
ready; errors retain the previous generation.

Supported first-integration views are single-sample camera raster passes and
full-slice directional cascades. Projected-light shadow atlases are rejected:
this Atom version does not publicly expose their actual subviewport. MSAA is
also rejected, rather than substituting pixel-center rays for sample rays.

## Current limits and remaining acceptance

### Opt-in full-resolution approximate preview

`r_silpomPreviewRayBudget 4096` enables a **quality-degraded preview**, with full
output resolution but fewer displacement rays. The budget is per displaced
batch, across its camera/light views, clamped to 32–65,536; it is not a global
scene budget or a frame-time guarantee. The existing 48.03 MiB allocation stays
unchanged. `0` (the default) restores strict native-pixel sampling and the
existing overflow diagnostic. No saved material or component value is changed.

The preview chooses the smallest power-of-two pixel-block size that fits every
view's projected rectangle into the budget. A ray samples each block, including
partial blocks at rectangle boundaries. Forward/depth/shadow consumers reuse
that result with nearest-block lookup at the full target resolution. This can
produce blocky silhouettes, stepped depth, missed small features and coarse
shadows. Only the sampled rays can be certified; the other pixels are explicitly
approximate. Exhausted/Invalid samples remain diagnostics, not invented hits.
The component status reports `APPROXIMATE preview`, block size and sampled rays.

`Tests/mesh_fullres_preview.py`, launched using Editor `--runpython`, opens the
saved `DefaultLevel` and requires the existing `SilPOM_Interactive_Test` entity.
It preserves entity transforms and bindings and does not save the level. It
checks strict sampling at 320×240, captures actual 1920×1080 output at 1,024 and
4,096 rays/batch, checks the GPU directory/allocation and restores an auto-sized
interactive viewport with the 4,096-ray preview enabled. Camera framing and
one 256-pixel shadow cascade are temporary session settings. Results, images,
hit buffers and spot GPU timings are in `user/SilPOMMeshFullResolution`.

The C++ `SilPOM.PreviewSampling` suite checks empty/odd/partial rectangles,
unrepresentable budgets, large dimensions and 10,000 randomized multi-view
budgets. This preview is not an issue #2 accuracy or production-performance
acceptance path.

### Remaining constraints

- Static triangle meshes, a single LOD and positive uniform entity scale only.
  Displacement amplitude does not scale with the entity. Collision is separate
  and undisplaced.
- Displaced batches require opaque StandardPBR source materials and nonzero
  height-UV tiling on both axes. The experimental shader is not a general
  replacement for arbitrary material types, transparency or cutout surfaces.
- Height storage is R32_FLOAT, R16_UNORM or R8_UNORM, normalized to [0,1], with
  one mip. Every covered bilinear cell is currently expanded on the CPU.
  Preprocessing and traversal budgets reject excessive inputs explicitly.
- The initial adapter requires one bindless-capable GPU and Atom mesh instancing
  disabled (`r_meshInstancingEnabled=false`). No per-triangle
  components or authored proxies are required. Sharing cooked resources across
  separate component instances is not implemented yet.
- All owned batches are excluded from Atom ray-traced effects and reflection
  captures; the experimental adapter disables motion-vector submission.
  Temporal AA, independent depth/shadow reference comparisons and game runtime
  have not yet been accepted. The bounded camera-motion regression below does
  not establish motion-vector or temporal-AA correctness.
- Shader hot reload and simultaneous multi-pipeline operation have not been
  accepted. Restart the disposable Editor after processing shader changes.
- Misses discard coverage. Exhausted/Invalid rays produce a magenta diagnostic,
  not transparent holes or certified surface hits. Diagnostic depth is not an
  accepted displaced-surface result.
- Open and tagged/ordinary boundaries are not capped. Comprehensive seam/fold
  validation, profile-source build dependencies, production ordinary model
  products and all reload/lifecycle acceptance cases remain outstanding.
- Native curved procedural DXR conformance is still outstanding; the existing
  native planar DXR tests are not a substitute.
- Do not infer scene-level viability from the tiny fixture or standalone ray
  batches. A target workload and frame budget must be agreed and measured.

No acceptance checkbox from issue #2 should be marked complete solely because
the surface asset imports or the shared standalone kernel passes its tests.

## Staged-renderer validation (2026-09-19)

The redesign completed graphics/compute pipeline setup and rendered the real
Blender-exported fixture on O3DE 2.7.0, DX12, Radeon RX 7900 XTX (driver
32.0.31041.1004). It did not reproduce the inline material-PSO stall. The final
Builder, Editor and runtime Gem DLLs built successfully; building the runtime
DLL is not a game-runtime test. New compute and material assets processed
successfully. The unrelated old transport-v1 fixture still fails intentionally.

The Editor reported three owned batches (one ordinary, two displaced), 27
bilinear fragments and 2,544 bytes of immutable traversal data. The test used
160x120 single-sample camera output and one 256x256 directional cascade with a
10 m shadow range. The range matters: at the earlier 100 m setting the tiny
fixture fell between shadow samples, so zero shadow hits were not accepted as
evidence.

| Check | Front | Grazing |
| --- | ---: | ---: |
| Certified camera hits, summed over displaced batches | 768 | 450 |
| Certified shadow hits, summed over displaced batches | 79 | 79 |
| Exhausted / Invalid samples | 0 / 0 | 0 / 0 |
| Visible camera samples with bit-identical raster depth | 768 | 318 |
| Visible camera samples with expected forward albedo | 768 | 318 |
| Visible shadow samples with bit-identical raster depth | 47 | 47 |

The grazing view has 340 unique curved-hit pixels before ordinary occlusion;
22 are covered by nearer geometry. No certified nearest camera hit is missing
from raster depth. Captured hit positions also agree with independently
reprojected depth within 7.7e-8 normalized depth units. Maximum reported ray
error across camera and light views was 2.72e-5 m, below the configured 5e-4 m.
Each light view has 58 unique curved-hit pixels: 47 bit-identical shadow-depth
matches and 11 nearer occluders, with none missing. Shadow reprojection error
is at most 1.05e-7 normalized depth units.
This verifies the compute-to-raster contract, not the surface solver against an
independent geometric reference.

Spot GPU timestamp captures put `SilPomIntersect` at roughly 5–7 ms and
`SilPomResolve` at 0.007–0.008 ms for this tiny fixture. These are individual
frames, not a benchmark distribution or a production frame-budget acceptance.
The 48.03 MiB scratch allocation and eager preprocessing are still experimental
costs. Do not extrapolate this result to a detailed character or full scene.

Validation artifacts are under `user/SilPOMMeshStagedValidation`; build and
standalone test logs are `build/mesh-staged-build-final.log` and
`build/mesh-staged-regression-final.log` in this Gem checkout. All four final
standalone suites passed: Core, CurvedCPU, Metadata and GPU (111.03 s total).
The GPU suite's planar DXR checks do not establish curved DXR conformance.
The smoke test
exits without saving DefaultLevel. Editor viewport restoration/shutdown still
emits the previously observed `Projected:SkinnedMeshes` missing-binding error
and AuxGeom render-attachment assertion; the complete Editor lifecycle is not
claimed clean.

## Camera synchronization follow-up (2026-09-19)

The original staged build displayed whole-object magenta only while the camera
moved. Its pass `FrameBegin` captured the previous frame's cached jittered view
matrices; raster used the matrices subsequently refreshed by `View::UpdateSrg`.
Moving preparation to `SetupFrameGraphDependencies` fixes that timing without
changing solver budgets, shader diagnostics or matrix-match tolerance.

The old-build moving-frame albedo capture contains 1,156 diagnostic pixels and
no fixture colors (`user/SilPOMMeshMotionBefore/motion_00_albedo.dds`). The rebuilt
Editor passed 12 paired albedo/hit-buffer captures during uninterrupted camera
translation and rotation: both fixture colors remained visible, with zero view
errors, zero Invalid records and zero unexplained magenta pixels. One capture
contained one magenta pixel at (98,75), matching an explicit Exhausted record
in displaced batch 1; all other captures had none. That bounded-solver limit
remains unresolved and visible. This is **camera synchronization** evidence,
not certification of every ray during motion.

Front/grazing certified-hit, albedo, camera-depth and shadow-depth checks also
passed again. Artifacts are under `user/SilPOMMeshMotionFinal`; the offline
capture recheck is logged in `build/mesh-camera-sync-captures.log`. The Builder,
Editor and runtime DLL builds passed (`build/mesh-camera-sync-build.log`), as
did all four standalone suites (69.29 s, `build/mesh-camera-sync-regression.log`)
and seven diagnostic-classifier tests (`python Tests/test_mesh_capture_analysis.py`).
The existing viewport-restoration/shutdown diagnostics are still present.

## Earlier inline-solver evidence (2026-09-19)

- Blender 5.0.1 exported the fixture using the actual installed add-on operators.
  SceneAPI validation and the new surface-asset build succeeded for that bundle.
- After moving the shared math into reusable headers and adapting shader symbols
  to AZSL, all four configured standalone CTest suites passed: Core, CurvedCPU,
  Metadata and GPU. The GPU suite retains planar DXR conformance; it does not
  provide curved DXR conformance or Atom-rendered mesh acceptance.
- O3DE 2.7.0's DirectX shader compilation succeeded, but full default asset
  processing failed in the Vulkan SPIR-V compiler. Several jobs reported
  `Internal compiler error: stack overflow`; others reached the compiler's
  300-second timeout. The remaining Vulkan compiler processes reached roughly
  5–6.6 GB each before the test stopped them. Logs are in the integration
  checkout's `build/mesh-assets-3.log`.
- With the verified temporary DX12-only settings, all new shader/material
  products processed successfully (`build/mesh-assets-dx12-3.log`). The Editor
  discovered the fixture's material slots/profile and accepted their bindings.
- The invisible-handle initialization deadlock was fixed by disabling new draw
  items in Atom's packet-update notification until all batches are ready.
  The subsequent test reached graphics-pipeline creation, but never completed
  the first frame. More than five minutes after entering mesh initialization,
  it was still inside the AMD driver. A non-invasive debugger snapshot shows
  `MeshFeatureProcessor::CreateInitJobQueue -> MeshDrawPacket::DoUpdate ->
  PipelineLibrary::CreateGraphicsPipelineState -> amdxc64.dll`; the main thread
  waits in `Scene::PrepareRender`. See `build/mesh-editor-driver-stack.log`.
- That test used an AMD Radeon RX 7900 XTX, driver 32.0.31041.1004, a 160x120
  camera viewport, one 256-pixel shadow cascade, four base triangles and a 2x2
  height map. Observed process peak working set was 14,369,161,216 bytes; a later
  private-memory sample was 14,867,820,544 bytes. The test Editor and its Asset
  Processor were stopped without saving DefaultLevel. This is **pipeline setup
  cost**, not a GPU frame-time measurement. No screenshot, depth/shadow-reference
  result, game-runtime pass or scene-level performance claim is available.
- The final disabled-by-default guard was checked through the real Editor
  component API. Both material assignments and the height-profile assignment
  succeeded; `IsReady` remained false and the component reported the explicit
  experimental-raster diagnostic without entering graphics-pipeline setup. The
  test exited normally and recorded `passed: true, guard_only: true` in
  `user/SilPOMMeshGuardValidation/result.json`. To repeat only this safe check,
  set `SILPOM_MESH_VERIFY_GUARD=1` in the shell launching the smoke test. This is
  a binding/guard check, **not** a raster acceptance pass.
- The old transport-v1 diagnostic fixture in TG is intentionally rejected by
  the version-2 production builder. It must be re-exported before an all-assets
  Asset Processor run can be clean; the test does not rewrite that old fixture.

## Redesign decision and remaining gates

The user approved the compute redesign after the measured inline-driver blocker.
Do not enlarge the fixture or present import/conformance successes as renderer
delivery. A workload/frame budget still needs agreement, followed by independent
depth/shadow reference comparisons, game runtime, curved DXR and the remaining
issue #2 acceptance criteria.

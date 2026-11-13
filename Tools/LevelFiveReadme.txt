Level 5A: learned relief appearance on explicit proxy depth
=========================================================

This default-off experiment removes detailed intersection work from selected
distant wall patches. It does not establish a GPU performance improvement.
No GPU benchmark is part of this implementation or its correctness tests.
Level 4A remains the independent learned-material comparison. Shared near
material/visibility encoding (4B) and bounded depth (5B) are separate default-off
experiments described in LevelFourFiveBReadme.txt. Coverage remains authored.

Controls
--------
On the tower's SilPOM Mesh component:
  Level 5A learned relief LOD (experimental): defaults off.
  Level 5A error budget (higher = more proxies): defaults 0.75 pixels;
      supported range 0.05-4. Larger values allow larger geometric differences.

The current candidate requires the canonical open-shaft wall, neural-only
local visibility, relief depth 0.5 and height-derived normals on for the near
path. Level 4A must be off: this isolates 5A using ordinary base color instead
of assuming the unmeasured Level 4 material codec has won. Saved levels and
materials are not changed. Unsupported combinations produce explicit status.

Representation selection
------------------------
Canonical wall batches are divided into pairs of adjacent coplanar triangles
where possible. Each pair is selected coherently. Authored material/profile
boundaries and exterior edges tighten the adjacent patch's error limit to
at most one quarter pixel, with a facing threshold of 0.5. Such boundaries
stay detailed until that stricter geometric limit is satisfied. Other materials,
stairs, trim and doorway reveals retain their existing representation.

Before render preparation, the CPU projects each patch's authored corners
and six displacement extrema through the current view/projection. A bound
includes the changing homogeneous W, the actual hit-buffer resolution and
viewport size. No hard-coded 656-pixel reference viewport is used. Taking the
larger resolution is conservative while a resize is in flight. Perspective,
camera changes, uniform transform and viewport resize affect eligibility.

Patches near clip singularities, behind the camera, or facing the camera by
less than 0.35 stay near. This deliberately keeps very grazing surfaces and
silhouettes on the existing near path. A patch enters the proxy below 75%
of the pixel budget and leaves above 100%. Selection is geometric; model
confidence never triggers another renderer. Multiple MainPipeline views
conservatively keep the wall near until a per-view policy is implemented.

Flags use immutable small host-buffer snapshots, replaced only when selection
changes. Each new bindless index is bound and compiled with the snapshot;
old buffers are retained across in-flight frames. Orphaning a DX12 buffer
changes its bindless index and must not leave a stale material constant.
This adds CPU policy, material-constant updates and upload
work, which must be included in the later end-to-end evaluation. The existing
distance-shell selection is bypassed for these wall batches: protected and
unselected patches remain detailed, independently of the old distance cap.

Removed work and retained work
-----------------------------
The near depth vertex shader rejects selected proxy patches before coverage
expansion or intersection. A separate proxy depth shader rasterizes the
authored triangle and publishes its raster depth, interpolated UV and owner
through the existing winning-hit record. That shader has no height texture,
intersection, descriptor, Newton or predictor includes/calls.

The forward shader chooses the representation from the prepared face flag
before material/normal work. A proxy skips fine-height normal reconstruction,
the ordinary detail-normal/roughness texture evaluations and Level 3 height
probes. It acquires filtered spatial features once, evaluates the surface
head once, and reuses those features for the small directional heads. A near
pixel retains the ordinary material and accepted neural-only visibility.
There is one expensive surface path per pixel, with no shaded-result blend.

The normal wall-hole backup remains available only for near patches. It does
not rerun detailed intersection for a selected proxy. Proxy depth and coverage
are authored geometry; no model predicts or validates either of them.
Proxy records are excluded from the neural-wall hole-fill donor classification,
so that the existing gap repair cannot grow or reconstruct their coverage.

External directional shadows, the existing dynamic-light/IBL plumbing and
runtime decal compositing are retained. Local appearance corrections apply
to the existing directional-light and spiral-light hooks. Punctual/area light
paths still use the decoded PBR normal and roughness through normal engine
lighting. A local shadow schedule of zero bypasses the directional head and
uses unit gain. Spiral brightness, window layout and light direction stay
runtime controls. Height-filtered decals may still read the source height;
those optional decal reads are separate from removed intersection/gradient
and Level 3 descriptor work.

Model
-----
Two RGBA8_UNORM images contain eight trained feature channels at 832 x 768
with all ten mips. Feature payload is 6,815,696 bytes; the 757 float32 decoder
coefficients are 3,028 bytes. These are payload sizes, not measured residency.
Original maps are retained for near pixels in these mixed batches.

The surface head is 18 -> 16 ReLU -> 3. Inputs are filtered features, view
direction, major/minor footprint/orientation and the local UV-to-world metric.
It outputs a bounded effective normal and roughness. The directional head is
22 -> 16 ReLU -> 2, adding light direction and physical reach. It supplies
bounded diffuse/specular corrections in [0,2], trained together with the PBR
outputs against filtered visibility times reflectance. They are appearance
corrections, not classifier confidence or a probability interpreted as depth.

The feature filter matches training: one to eight directional taps from each
trilinear, clamp-addressed feature image, followed by a single surface decode.
An analytic projected-plane UV Jacobian supplies the footprint and base-color
gradients even when neighboring pixel lanes choose different representations.
It is checked against independent projected-plane finite differences on GPU.
All feature mips are trained. No shadow direction reacquires spatial features.
FP32 arithmetic and ordinary filtered sampling work with SM6.2 and SM6.7;
packed dot products/raw gathers are separate backend experiments.

Training and its limits
-----------------------
Tools/appearance_reference.cpp is an offline CPU renderer built as
silpom_appearance_reference.dll. It traces the detailed affine displacement,
samples source color/detail normal/roughness at the hit, computes dense local
height visibility and a GGX/Schlick PBR response, then filters 64 stratified
subpixel samples. Training varies view/light direction, reach and anisotropic
footprint; it does not learn final RGB for one sun or spiral-light arrangement.
The native reference is tested against an independent flat Lambertian oracle,
flat-footprint convergence, and a ridge with opposite light directions.

This is a local material reference, not the engine's entire scene renderer.
It uses the selected triangle's detailed relief and excludes training queries
with insufficient traced coverage or capped visibility. It does not predict
that coverage. External scene occlusion is retained through runtime shadows.
Source PNGs and a GGX/Schlick teacher differ from GPU texture compression and
the engine's exact BRDF/filter implementation. Very grazing views are excluded
by the runtime geometry policy. Fine silhouette, boundary and motion quality
still require inspection; correctness checks alone are not acceptance.

The fine fit uses 16,384 initial training queries, 1,500 optimizer steps and
an independent set of 8,192 held-out queries (4,830 usable for the local
reference). Triangle-coverage filtering leaves no accepted fine-fit queries
for the coarsest two mips. Tools/train_appearance_tail.py closes that gap:
it continues each source face's physical UV metric as a local flat relief
surface, renders 2,048 coarse training and 1,024 held-out queries, and fits
feature mips 7-9 while freezing the decoder and mips 0-6. It verifies training
participation for all ten mips and rejects a fine held-out regression before
promoting a version 2 runtime bundle. Version 1 is a provisional fine fit
and is rejected by the runtime. This local continuation is an appearance
reference; runtime depth/coverage still come from the authored proxy.
LevelFiveQuality.json records absolute filtered-response error
and a flat PBR comparison, not frame rate or engine image-quality scores.
Do not interpret those reference errors as proof of production visual quality.

Reproduce and test
------------------
Build the standalone CMake targets, then with NumPy, Pillow and PyTorch:

  python Tools/relief_appearance.py <TGProject> <Release-DLL-folder> <scratch-output> --queries 16384 --steps 1500 --samples 64
  python Tools/train_appearance_tail.py <TGProject> <Release-DLL-folder> <scratch-output>/wall.l5app <scratch-tail> --steps 1500

Copy scratch-tail/wall.l5app to TGProject/Assets/SilPOMLearned/wall-appearance.l5app.
Asset Processor creates wall-appearance.silpommaterial. The builder verifies
SHA256 of the original basecolor/normal/roughness/height PNGs and tower sidecar.
Retrain when those sources change. Runtime/builders do not need PyTorch.
Large checkpoints, packages, training sets and captures remain in ignored
scratch/user folders. Only the runtime source encoding belongs in TG assets.

CTest covers existing geometry/visibility/material behavior plus the projected
bound, viewport scaling, hysteresis, boundary/grazing guards, bundle validation,
reference rendering, independent feature filtering, and CPU/GPU heads on both
shader models. silpom_appearance_gpu_tests accepts a .l5app after the shader
path to verify the actual trained coefficients. It collects no GPU timings.

Tests/Run-LevelFiveSmoke.ps1 opens a dedicated editor. Its checks are
Level 4 hit regression, incompatible-mode rejection, visible proxy raster hits,
pixel bounds during resize/approach, dynamic sun response and a decal stamp
with undo. It restores controls, camera and lighting without saving and never
invokes the benchmark capture service. Close Editor/Asset Processor first.

Validation of this candidate passed: 30 standalone test groups and the editor
smoke. The trained heads agreed with CPU on 4,096 GPU inputs per shader model
(maximum output difference 1.79e-7). In the front view, 2,213 selected pixels
all used proxy raster hits, with zero refined hits. The sun change affected
those proxy pixels and the decal stamp changed 51 of them; its center picker
hit a selected proxy. A resolution increase reduced selected faces from 306
to 158, and an approach to 8 m returned the wall entirely to the near path.
Original settings/light/camera were restored and the saved level hash matched.
LevelFiveValidation.json contains the compact correctness record; large
screenshots/readbacks remain in ignored scratch storage.

Keep the baseline, 4A and 5A selectable until matched quality and complete GPU,
CPU and memory costs can be measured. 5A is useful only if the work removed
outweighs its policy, rasterization, filtering and decoder costs.

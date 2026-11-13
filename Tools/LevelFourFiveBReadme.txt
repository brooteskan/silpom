Level 4B shared visibility and Level 5B bounded proxy depth
=========================================================

These are separate, default-off experiments, implemented at the user's request
before benchmarking. No GPU speedup is established. The baseline, 4A and 5A
remain selectable. The saved VueLiveLink prefab is not modified by installation.

Controls on the tower's SilPOM Mesh component
--------------------------------------------
  Level 4B shared material / visibility (experimental)
  Level 5B bounded proxy depth (experimental)

Both require the calibrated neural-only local-shadow mode and depth 0.5.
4B replaces 4A: turn 4A off. 5B requires the 5A LOD selector to be enabled;
the existing 0.75-pixel budget and geometric hysteresis are unchanged.
4B and 5B can be enabled together. Near wall pixels use 4B, selected distant
patches use the 5B depth stage followed by the existing 5A appearance stage.
Very grazing/near patches stay detailed according to the existing CPU policy.

Level 4B
--------
The original 4A material weights and eight-channel feature mip chain are
unchanged. Four additional UNORM8 channels describe cardinal height horizons,
with all eleven filtered mips. Three RGBA8 images are acquired together during
the one material decode. The four horizons plus decoded normal, roughness
and luminance form a compact state reused for each directional/spiral light.

A 20 -> 12 ReLU -> 1 sigmoid head receives that state, light direction,
UV-to-world metric, affine center displacement direction, reach and UV center.
It predicts filtered local visibility, rather than classifier confidence.
There is no Level 3 height-descriptor acquisition, material re-decode or atlas
resampling per light. A zero shadow schedule returns unit visibility before
the head; back-facing enabled light returns zero. Other light plumbing,
external shadows, decals, hit reconstruction and fine height normals remain.
The supported directional/spiral hooks are the same hooks used by Level 3;
this does not add local relief visibility to all engine light types.

wall-shared.l4shared has magic SILPSHR4, version 2, 575 float weights and
40,896,760 total bytes. The third feature image adds about 13 MiB to 4A.
This extra acquisition and memory must count against the removed height
probes in the later measurement. It is not an assumed compression win.

Level 5B
--------
The selector chooses the far representation before any detailed intersection.
A separate depth variant rasterizes the authored proxy triangle. It filters
the frozen 5A feature atlas once and runs a 23 -> 16 ReLU -> 1 sigmoid head.
Inputs are features, view, footprint, UV metric, affine center displacement
direction and UV. The output is a height fraction constrained to [0,1].

The head uses the local constant-direction displaced plane. With geometric
normal N, center direction D, view direction V and h=scale*(fraction-reference),
the one analytic ray offset is t=h*dot(N,D)/dot(N,V). The UV coordinate is
shifted analytically on that plane and the corrected depth/UV/owner is
published through the existing winning-hit record. Forward/decal picking
therefore consume the same corrected hit. The displacement scale is in world
metres, as in the existing renderer; uniform object scale affects the base UV
metric but does not multiply relief height. There is no height read, Newton
step, march, validation, confidence test or tracing recovery in this stage.

Coverage and silhouettes remain authored geometry. Coverage prediction is
deliberately not implemented in this 5B experiment. A depth offset alone
cannot recover detailed silhouettes. The existing stricter boundary policy
and grazing guards remain important. Local frozen D is an approximation to
the near path's spatially affine D. The original projected error budget is
retained conservatively; the head does not justify raising that budget.

wall-depth.l5depth has magic SILPDEP5, version 2, 1,158 float weights and
6,820,396 total bytes. The original 757 appearance coefficients and complete
ten-mip atlas are frozen. The extra geometry head has 401 coefficients.
The forward pass reacquires appearance features at the corrected winning UV;
5B adds this depth-stage sampling/head work compared with 5A. Include it in
later cost measurements. Shared near+far mode retains the ordinary basecolor
image for the far path, while replacing near material evaluation with 4B.

Training and reproducibility
----------------------------
  python Tools/train_shared_depth.py <TGProject> <native-DLL-folder> <scratch-output> --queries 16384 --steps 3500 --samples 16

Use NumPy, Pillow and PyTorch offline. The DLL is silpom_appearance_reference.
The expensive teacher traces/filters the detailed source heightfield on a
continued local patch using the face's UV metric and center displacement D.
Incidental triangle boundaries do not become coarse-footprint coverage holes.
4B fits dense height visibility at an already resolved surface coordinate;
5B fits filtered visible height along view rays. Flat height/visibility and
opposite-light ridge cases have independent reference oracles. Queries vary
view/light/reach and anisotropic footprints; all feature mips participate.

The held-out set is independently sampled. Absolute 4B visibility MAE is
about 0.080 (95th-percentile absolute error 0.318). 5B height MAE is about
0.0030 m at the calibrated 0.05 m relief scale (95th-percentile about 0.0081 m).
These local-reference errors are not engine image-quality scores. Neither
head is a proof of production visual quality or motion stability.
LevelFourFiveBQuality.json records the exact candidate and payload hashes.

Copy the two bundles into TGProject/Assets/SilPOMLearned. The builder verifies
SHA256 of basecolor/normal/roughness/height PNGs plus the tower mesh sidecar.
Header kinds cannot be interchanged between 4A/4B/5A/5B. Provisional version 1
bundles, wrong sizes and nonfinite weights are rejected. Runtime has no torch
dependency. The source models belong to TG's asset repository; datasets,
packages, checkpoints, captures and build outputs remain ignored scratch data.

Verification
------------
CTest covers codec rejection, bounded plane offsets, independent offline
references and CPU/GPU conformance on SM6.2 and SM6.7. The GPU executable
silpom_appearance_gpu_tests also accepts an actual learned bundle as its
optional second argument; it compares 4,096 inputs without timing capture.

Tests/Run-LevelFiveSmoke.ps1 adds 4B hit invariance, 5B changed raster depth,
combined-mode selection, dynamic lights, decal picking/paint/undo and the
existing resize/approach checks. By default it restores controls and exits
without saving. -LeaveOpen leaves both B modes active for user inspection,
with depth 0.5, 5A selection on and 4A off; it still restores temporary light
changes, removes its decal canvas and checks that the saved level is intact.
No benchmark capture service is invoked.

This candidate passed 30 standalone groups and the editor check. All 2,213
comparable front-view proxy pixels changed depth, while 4B left the winning
hit unchanged. Combined rendering, geometric resize/approach selection, sun
response and a decal stamp/undo passed; 33 proxy pixels changed from the stamp
with the brush preview hidden. Captured representation flags are used for
pixel masks because entity edits can reallocate mesh-owner IDs. The depth
filter uses the already-bound material clamp sampler, with no depth-pass SRG.
LevelFourFiveBValidation.json records the compact result. The saved level
hash matched; the test can leave both modes enabled in the open editor.

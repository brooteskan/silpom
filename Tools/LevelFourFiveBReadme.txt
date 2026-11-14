Level 4B shared visibility and Level 5B bounded proxy depth
=========================================================

These are separate, default-off experiments, implemented at the user's request
before benchmarking. No GPU speedup is established. The baseline, 4A and 5A
remain selectable. The saved VueLiveLink prefab is not modified by installation.

Controls on the tower's SilPOM Mesh component
--------------------------------------------
  Level 4B shared material / visibility (experimental)
  Level 5B bounded proxy depth (experimental)
  Neural directional visibility (off = cheaper)
  Learned proxy directional response (off = cheaper)

Both require the calibrated neural-only local-shadow mode and depth 0.5.
4B replaces 4A: turn 4A off. 5B requires the 5A LOD selector to be enabled;
the existing 0.75-pixel budget and geometric hysteresis are unchanged.
4B and 5B can be enabled together. Near wall pixels use 4B, selected distant
patches use the 5B depth stage followed by the existing 5A appearance stage.
Very grazing/near patches stay detailed according to the existing CPU policy.

Directional enablement is independent of the legacy relief step count, which
remains 0 on trained profiles. Neural directional visibility enables detailed
Level 3/4A/4B local visibility with the fixed trained descriptor schedule 16;
4B uses its shared state. It respects the engine shadow-enable option.
Learned proxy directional response enables the frozen joint visibility-times-
reflectance correction on 5A/5B. Its visibility and BRDF contributions cannot
be independently separated from this model. This intrinsic response has its
own switch and remains independent of external scene-shadow enablement and
of the near-visibility switch. Both new controls default off, preserving the
previous directional behavior. Spiral lighting retains its own schedule.
Neither control can enable a marcher; both require neural-only mode.

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
resampling per light. A disabled near-directional control or zero spiral
schedule uses unit visibility; back-facing enabled light returns zero. Other light plumbing,
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
The forward pass restores the original authored-plane query on the winning
camera ray. It samples the frozen appearance features and base-color factor
at that proxy UV, with the proxy-plane footprint and reciprocal clip W.
Resolved depth, position and UV remain the inputs to depth integration,
lighting positions, picking and height-filtered decals. These coordinate
roles are distinct: the frozen 5A target is appearance seen through a proxy
location, including the detailed relief's shift along that viewing ray.
Base color uses clamp addressing, matching the reference, with anisotropy
capped at eight; feature filtering retains its explicit clamp sampler.
GPU mip/compression/filtering still approximate the offline pixel integral.
5B adds depth-stage sampling/head work compared with 5A. Include it in
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

The fitting pipeline has three independently seeded partitions. Only training
labels drive optimization. Checkpoint selection uses validation MSE. The
selected model is frozen before the final test partition is read or rendered;
test labels never choose a checkpoint. One fifth of each partition samples
near material/exterior boundaries. All mips participate. Reports separate
supported grazing views (cos V below 0.5, with runtime minimum 0.35), grazing
lights, large footprints, long finite reach, unbounded reach and boundaries.
Boundary labels still use the continued local reference, not silhouette coverage.

The final test has 4,096 queries per head. Shared-visibility MAE is about 0.0862.
5B height MAE is about 3.07 mm; mean error along the viewing ray is about
5.05 mm versus 15.66 mm for authored-plane depth, under the same local reference.
The test report also quantifies resolved-UV error. These are local-reference
errors, not engine image-quality scores or proof of motion stability.
The former 0.080 visibility / 3.02 mm height figures came from checkpoint
selection validation. They are labelled as historical validation in the report.

Frozen inputs are verified against their own source contracts before training:
4A hashes the three material PNGs; 5A additionally hashes height and the mesh
sidecar. A stale encoding is rejected instead of relabelled with current sources.
The report records the exact frozen input bundle SHA256, its source hash,
the output hash, renderer hash and reference-cache contract. Cached references
also verify their partition seed and renderer/query contract.
LevelFourFiveBQuality.json contains the full partition/subset metrics.

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


Review regression checks also exercise the depth/UV solve against an independent
plane reference on both shader models, with oblique rays, sloping/ridged spatial
material signals and uniform scale. The editor compares the albedo, effective
normal and roughness attachments between 5A and 5B during lateral camera motion.
Separate unit-response versus learned-response captures isolate each directional
head with spiral light disabled and legacy relief steps held at zero. A sun
intensity change alone is insufficient evidence that the learned head ran.

Performance remains unmeasured. Measure 4B against 4A plus active Level 3 at
several light counts, including no local response. Compare ordinary and learned
proxy shading with identical selection flags, then assess old/new LOD policies
separately at matched quality. Include CPU selection, flag uploads, material
compiles and memory. Judge 5B's quality benefit against its extra depth/filter
work and the loss of conservative depth semantics (SV_Depth versus 5A's
SV_DepthLessEqual), including occluded views. No benchmark was added here.

The reviewed candidate passed all 30 standalone groups and the expanded editor
check. Both directional heads contributed with legacy steps 0: 100,673 near
and 2,191 proxy HDR pixels changed, while repeated unit-response captures were
identical. During three lateral camera positions, 5A/5B albedo, normal and
roughness attachments had mean absolute differences below 1.1e-5. The check
requires mean below 2e-4 and the 99th percentile below 0.004, allowing small
quantization/occlusion-boundary differences. Decal paint/undo and geometric
resize/approach checks passed. This confirms contracts and contribution, not
full-scene reference accuracy or speed. The saved level was unchanged; both
modes and both new directional controls were left enabled in the open editor.

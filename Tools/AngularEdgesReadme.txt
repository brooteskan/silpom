Four-cap angular edges and diffuse accessibility experiments
==========================================================

Lighting estimator 5 is a separate, retrained four-occluder candidate. Existing
estimator IDs, coefficients and Current routing remain intact. Diffuse ambient
accessibility is a separate opt-in control, serialized false by default. Enable
it on any directional-field/cap model; enabling from a route without a hemisphere
model selects the new angular-edge candidate automatically. Selecting Current or
physical per-ray lighting turns this experiment off.

Boundary representation
-----------------------
Each receiver decodes four normalized axes, four nested cosine thresholds per
axis, and four independent edge widths. The head is 20 -> 32 ReLU -> 32 outputs.
Sigma is shared across physical reaches per cap and ranges from 0.1 to 8 degrees.
For unit L and axis a, the normalized margin is

  (clamp(dot(L,a),-1,1)-c) / sqrt(max(1-c*c,1e-6)) / sigma

Visibility is sigmoid of the negative maximum margin. This is a first-order
angular-distance approximation about the boundary, not exact acos distance deep
inside a cap. Its boundary slope is in angular units, independently of radius.
Duplicate/overlapping blockers remain an idempotent union. Ordered thresholds
and the shared positive width preserve reach monotonicity.

A synthetic five-degree cap with a quarter-degree width can be strongly opaque
at its center while retaining a five-degree boundary. The original fixed-cosine
beta-32 evaluator cannot express that combination. Existing models keep their
original evaluator; no shipping coefficient is reused with changed semantics.

Training and evaluation
-----------------------
Tools/train_angular_edges.py reuses the verified full-wall FP64 continuous-cell
labels, frozen material encoding, repeat addressing, physical reaches and UV
block partitions from the previous experiment. It warm-starts the old four-cap
head, adds independent widths and retrains the combined union end to end. It
selects on calibration data, then reads the final test partition after freeze.

Tools/AngularEdgeQuality.json records training source, label contract, selected
checkpoint, one-texel/filtered subsets and coefficient fingerprint. Recorded
arbitrary-angle test MAE is 0.0818 versus 0.1138 for the original four caps.
Learned median widths differ by footprint (about 2.13 degrees for one-texel
receivers and 6.16 for filtered receivers). This removes the representation's
forced radius/softness coupling; it does not guarantee every predicted small cap
will be opaque or that the fitted geometry is correct at every receiver.

Tools/evaluate_angular_edges.py performs an independent post-freeze sweep of
six held-out receivers at two footprints, two elevations and two reaches. These
curves are visibility-only, without BRDF/proxy changes. They are diagnostic
samples, not additional checkpoint selection. Its ambient error compares the
same 32 cosine directions with the teacher across all 2048 held-out receivers,
separately from Monte Carlo quadrature error.

Diffuse ambient integration
---------------------------
Accessibility averages 32 cosine-hemisphere directions in the anchored base
frame, using relief-top exit support. N is independent of the cap count K. This
is an isotropic diffuse-accessibility approximation, not an environment-radiance
integral or a specular occlusion estimate. It adds no height probes or marching.

Atom normally finalizes diffuse AO over direct lighting too. The experimental
forward hook therefore keeps the new factor out of that common multiplier,
applies it only to diffuse IBL contributions, and exports it in diffuse-GI
albedo RGB after direct-light finalization. Existing external SSAO behavior is
retained. Specular lighting/occlusion and albedo alpha are unchanged. The decal
application order remains unchanged.

Ambient and angular-edge shaders are separate compiled routes. AO-off routes
do not compile the ambient loop; existing comparisons do not compile the new
angular head. Explicit loop annotations prevent compiler expansion of nested
direction/network loops. This separation is important on the tested AMD driver:
an early combined fallback shader stalled graphics-pipeline compilation and
hung the isolated test Editor. Thread stacks recorded the AMD compiler worker
and the main thread waiting on the render job. That build was not installed.

Visual assessment
-----------------
Tests/AngularEdgesEditorSmoke.py first captures Current routing with frozen
reference settings. It then disables Level 5 for near-wall comparisons, disables
direct scene lights for an environment-only AO comparison, and sweeps the same
directional light across the old and new four-cap routes with fixed material,
geometry and light-enable controls. HDR direct, specular, albedo and normal
attachments distinguish diffuse accessibility from double direct shadowing.
The test never saves the level and restores temporary controls.
It temporarily enables matching sun lighting channels and verifies quaternion
rotation updates; the saved sun channels are not changed. The opaque pipeline
exports diffuse accessibility through its GI albedo attachment. With the
tower's current missing baked GI data, environment-only frames can be black:
those frames validate the albedo routing, not visible ambient illumination.
Run Tests/Run-AngularEdgesSmoke.ps1, then Tests/AngularEdgesVisualTests.py on
its output using a NumPy-equipped Python to verify the captured attachments.

Do not infer visibility quality from distant proxy brightness alone: comparison
proxies still omit the frozen joint visibility/reflectance gain and are not an
identical-reflectance A/B. Separate proxy reflectance training remains future
work. Field interpolation also remains a separate experiment; the existing
32-direction choice still uses nearest-direction lookup.

The new lighting paths are march-free. The renderer as a whole is not: the
approved camera-hit recovery deliberately retains its bounded ordered sampling
at grazing model-4 intersections. This experiment does not alter that exception.

No GPU speedup is claimed. Diffuse accessibility adds optional GPU work, and the
new candidate remains experimental rather than replacing the approved default.

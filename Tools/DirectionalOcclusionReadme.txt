Directional occlusion: switchable representation experiment
==========================================================

Status: live experimental comparison plus the original offline prototype.
The SilPOM Mesh component now exposes Lighting estimator. Current routing (0)
is the serialized default and uses the approved forward shaders; experimental
comparisons use separate forward shaders. Camera depth and nearest-hit recovery
are independent. No saved tower settings or existing learned bundles are changed.

Live choices: Current routing, Physical per-ray neural, 32-direction neural
field, 4 neural angular occluders and 8 neural angular occluders. Comparisons
require Level 4B, neural-only mode and calibrated depth 0.5. They replace local
direct-light visibility on the canonical wall, including the central column;
other materials retain current routing. Light enable switches retain their
meaning. The detailed reference remains offline only.

The new heads cover wall receivers through frozen full-material features, UV,
local metric/displacement direction and footprint context. They are trained
models, distinct from the six freely fitted points in the original inspection
tool. This is a comparison build, not promotion over the approved estimator.

For proxies, comparisons use visibility with the existing decoded proxy BRDF
and omit the joint Level 5 visibility/reflectance light gain. They do not stack
an extra AO term onto that joint gain. This changes the response factorization,
so distant proxy comparisons are not a pure visibility-only A/B of identical
reflectance. Separate reflectance training remains future work.

Full-surface training: Tools/train_directional_estimators.py. Frozen weights:
Assets/Shaders/SilPOM/DirectionalEstimatorWeights.azsli. Quality/provenance:
Tools/DirectionalEstimatorQuality.json. Runtime rejects changed shared feature
generations; retraining must update weights, contract header and frozen test
fixtures together. Original learned bundles remain byte-identical.

Each head has 20 inputs and 32 hidden ReLU units. Four/eight caps decode axes
and four ordered cosine thresholds once per receiver. Soft maximum-union
visibility cannot double-count overlaps. The field reuses hidden state and
evaluates the nearest canonical direction for each light. Both enforce nested
reach support. Finite reach interpolation is experimental; the saved tower's
queries use 0 (relief-top exit) and 0.30 metres. No new traversal, confidence
fallback or camera-intersection recovery is added.

Training uses 12,288 receivers, 2,048 calibration and 2,048 final test receivers,
separated by hashed 32x32 UV blocks. Continuous FP64 cell labels use 16 footprint
samples, 64 directions and four reach bands. Seven uncertain training labels are
masked. Final test labels are generated after calibration checkpoint selection.

Nearest-direction field lookup has substantial angular discretization error.
The learned eight-cap model currently generalizes worse than four caps. See
held-out random-angle and one-texel/filtered subsets in the quality record;
canonical field-direction error alone is not an arbitrary-light error metric.
No GPU speedup or visual-equivalence claim is made for the new modes.
The live teacher uses the wall's repeat addressing, including footprints and
rays crossing UV texture borders; a thin-blocker boundary oracle tests this.

Tests/DirectionalEstimatorModelTests.py [standalone build directory] checks
frozen Torch outputs, reach monotonicity and GPU shader models 6.2 / 6.7.
Tests/Run-LightingEstimatorSmoke.ps1 captures every live route and distant
proxies, restores controls and never saves the level. NearestHitVisualTests.py
checks the current captures against the approved image/hit-buffer manifest.

Direction
---------
Decode a receiver-local directional model once, then query it for many lights
and ambient accessibility. Representation capacity K and query direction count
N are independent. Fit the combined directional coverage, not primitive IDs or
the scalar value of a packed integer. Four and eight caps are fitted separately
against the same teacher; the smaller model never deletes half of a larger fit.

The angular-union idea is informed by CORTAO's public reference implementation:
https://github.com/theagentd/CORTAO
https://raw.githubusercontent.com/theagentd/CORTAO/main/src/shaders/CORTAOMainPass.h
This experiment uses authored material relief for its labels.

Selector contract
-----------------
Tools/directional_occlusion.py defines stable choices and EstimatorSet dispatch:
  0 Current routing (live default; preserves existing 4B/5 policy)
  1 Existing physical per-ray neural classifier
  2 32-direction field
  3 Four angular occluders
  4 Eight angular occluders
  5 Detailed reference (inspection/teacher only)

Unavailable estimators raise an explicit error; there is no silent algorithm
fallback or recovery marcher. The prototype exposes choice 1 plus fitted 2/3/4,
and displays the teacher alongside each. The existing per-ray classifier is a
comparison baseline, not a claim to reproduce the tower's entire 4B/5 shading.

The live selector is independent of geometry prediction. Offline callbacks
remain useful for freely fitted representation experiments; the live entries
use the trained full-surface heads described above.

Teacher and representation
--------------------------
The initial experiment uses six height-stratified receivers on the central
column, at the saved relief-depth multiplier (0.5 in the recorded run). The
frame is UV-u tangent, bitangent and base face normal, attached to the material;
camera orientation is absent. Native FP64 continuous bilinear-cell tracing
uses the existing frozen-local-displacement and continued-material lighting
contract. It is not a trace of neighboring scene objects or all curved faces.

Physical reach bands are 25 mm, 100 mm, 300 mm and relief-top exit. The final
band's API cap is zero, meaning no extra cap, not zero physical distance.
Shared axes and nonincreasing cosine thresholds enforce nested blocked regions.
The prototype queries explicit bands. Arbitrary between-band light distances
still need a defined/validated interpolation or distance-bearing representation.

Each cap set has K normalized axes and K x 4 cosine thresholds: 7K coefficients.
Point visibility is the deterministic union of angular cap tests. Footprint
visibility is fitted to a four-texel square using 2x2 receiver quadrature; it is
a visibility fraction, not classifier uncertainty. The filtered evaluator
smooths the maximum union margin, so duplicate/overlapping blockers cannot
darken the same direction twice. Its fixed soft-edge basis remains approximate.

The 32-direction comparison stores 32 cosine-hemisphere visibility components
per band, with nearest-direction lookup. It intentionally exposes coarse
angular resolution and stepping. Packing is only an optional binary format.

Fitting and inspection
----------------------
Per receiver/footprint: 512 fitting directions, 256 calibration directions,
512 held-out directions, 256 additional cosine directions for ambient checks,
1536 inspection directions and 32 canonical field directions. Shallow lights
are deliberately sampled. Both capacities use identical data and reach bands.
Only calibration angles select among three fit restarts; held-out angles never
select a checkpoint. This tests representation capacity at fixed receivers,
not learned generalization to new material locations.

Run with a Python containing NumPy/Pillow and the native standalone tools:
  python Tools/fit_directional_occlusion.py <open-shaft experiment.json> <DLL-dir> <output>
  python Tests/DirectionalOcclusionTests.py

--reuse reuses labels only if their teacher contract fingerprints match. Source
hashes, native evaluator, height/mesh identities, frame, footprints and physical
support are recorded with the data. Model coefficients and reports stay under
ignored scratch, outside shipping Assets.

Recorded experiment: scratch/silpom-directional-occlusion/fit/ from workspace.
The interactive comparison allows estimator, receiver, reach, footprint and
light-direction switching. Light/dark presentation and 320 px layout were
checked in a headless browser. No GPU performance claim is made.

Original offline prototype acceptance gates (still relevant to promotion)
---------------------
Inspect narrow openings, shallow-angle boundaries and filtered-footprint bias.
The initial eight-cap point fit is promising, but filtered fits are currently
less accurate than the existing classifier on these six receivers. The trained
full-surface decoder does not reproduce freely fitted parameters everywhere.
Train union visibility end-to-end using a spatial neighborhood encoding, such
as shared material features; four samples along one light ray are insufficient
as the sole hemisphere descriptor. Keep spatial receiver splits independent.

Before promotion, test continuity across neighboring receivers, triangle/UV
boundaries and representation/footprint transitions. Rigid frame invariance is
tested; these six stationary receivers do not establish cross-triangle
continuity. Fit lower capacities against the same filtered target.

Integrate this as the visibility component, with matched near/shared/proxy
physical reach. Existing Level 5 appearance contains filtered visibility with
reflectance: it needs consistent retraining/factorization, not another AO
multiplier. Benchmark only after the representation has visual acceptance.

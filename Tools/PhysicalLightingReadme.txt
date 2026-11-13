Learned lighting: common physical shadow reach and live bundle replacement
========================================================================

The neural-only near, 4B shared, and 5A/5B proxy lighting targets now use
one physical support definition. With a normalized light direction, the ray
ends where its biased height reaches the relief's global top. A positive
maxDistance additionally caps this world-space interval; zero means no
additional cap. Nonpositive height rate is shadow. Disabled controls still
produce unit response without acquiring height samples.

Assets/Shaders/SilPOM/PhysicalShadowReach.azsli is shared by the near query
preparation and the offline shared/proxy teacher. The directional schedule of
16 selects descriptor positions, not a 16-texel physical cutoff. Changing the
schedule can change approximation error but cannot change the teacher interval.
Near acquisition remains at most five height quads, with no runtime marcher,
uncertainty recovery, or continuation. Four samples can still miss thin ridges.

Retraining
----------
The physical-reach near classifier has its own coefficients in
NeuralVisibilityPhysicalWeights.azsli. The historical hybrid retains the
original NeuralVisibilityWeights.azsli and its original interval semantics;
Baseline and Gather are also retained. Their results are not matched-target
quality measurements for the new full-reach classifier.

Tools/train_physical_visibility.py trains at relief depth 0.5 on all four
profiles, including the actual directional zero-cap / schedule-16 queries.
Each profile has 100,000 training, 20,000 calibration and 20,000 untouched
test queries. Training runs a fixed 6,500 optimizer steps; calibration selects
only the binary cutoff. Neither training nor selection uses final test results.
Reports record source, native evaluator, height, and geometry fingerprints.
Datasets, coefficient checkpoints and captures belong under ignored scratch.

The 4B and 5A/5B teachers already used relief-top exit for zero cap. Refactoring
their interval into the common helper preserves those targets, so those frozen
bundles are retained, not relabelled as retrained models. Their existing errors,
including grazing-light approximation error, remain relevant. The .l4mat,
.l4shared, .l5app and .l5depth source bytes are unchanged after verification.

Regression coverage
-------------------
VisibilityOnlyTests.py and AppearanceReferenceTests.py use the same synthetic
1 mm texel / 5 cm relief fixture: a ridge 23-26 texels away is beyond the old
16-texel interval. The physical near descriptor sees it; the shared and proxy
teachers also shadow it. An explicit 16 mm cap excludes it. Separate CPU checks
verify count-independent physical intervals, finite caps and back-facing rays.
GPU parity includes the actual schedule 16 under both SM 6.2 and SM 6.7.

Run-LevelFiveSmoke.ps1 walks the representation transition with both learned
directional controls enabled. This is a renderer execution check; it does not
claim exact visual equivalence between independently approximated representations.
The synthetic blocker regression isolates their physical target agreement.

The same editor check changes a valid decoder coefficient in each of the four
bundle formats while its component remains enabled. Asset Processor reloads it;
the published generation and material readback must change without configuration
toggles or restart. Restoring the source must restore both. Python finally and
the external PowerShell runner both restore original source bytes, including
after a failed check. Active generation IDs now include enabled learned bundles.

The saved VueLiveLink level and default baseline settings are preserved.
No performance benchmark was run for this revision.

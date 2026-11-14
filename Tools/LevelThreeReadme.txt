Level 3 local relief visibility experiments -- 2026-11-12

In the VueLiveLink level, select Irish Fieldstone Tower (Open Shaft), then
expand the SilPOM Mesh component. Local shadow method selects the original
marcher, Gather, the historical hybrid, or a separately compiled neural-only
experiment. Baseline remains the default. Hybrid shadow confidence defaults
to 0.995; higher values send more rays to the hybrid's Gather fallback.
The neural-only method has no marcher and uses calibration-selected binary
thresholds. See NeuralOnlyReadme.txt and NeuralOnlyEvaluation.json.
Camera intersections and external shadows use
their existing paths. Existing saved levels inherit Baseline.

The historical hybrid tests four existing march positions, then evaluates a
10 -> 12 -> 1 squared-ReLU network for supported open-shaft height profiles.
An uncertain prediction marches the remaining positions. Small sample counts,
unsupported profiles, signed negative relief and out-of-domain travel use
the Gather marcher. Predictions are approximate: confidence is a learned
score, not a mathematical visibility guarantee.

Tools/level_three.py generates native FP32 descriptors, trains coefficients
against Tools/visibility_reference.h's independent FP64 bilinear-cell trace,
and reports disjoint calibration/held-out results. The continuous reference
also finds thin blockers between the original march positions. Current
coefficients were trained on 300,000 requests per profile; each calibration
and held-out set contains 50,000 requests per profile.

All 22 standalone regression suites pass. Visibility tests include continuous
quadratic occluders/tangencies, signed relief, repeat/clamp addressing, disabled
shadows, budgets and fallback probe reuse. Each SM 6.2/6.7 GPU test checks
98,304 CPU/GPU visibility results, including all four models, Gather and
neural branches. Visibility and branch events agree; maximum probability
difference is 6.80e-6. A lit physical-reach endpoint may require one extra
non-blocking query because CPU/GPU reciprocal rounding differs.

The source Editor build and O3DE shader processing succeeded (30 processed
assets, zero failed jobs). VueLiveLink was rendered with all three methods;
component settings, camera and CVars were restored, without saving the level.

Historical RX 7900 XTX results favor Gather rather than the hybrid method at
both 9 and 64 samples. Keep Baseline for comparisons, select March with Gather
for the measured cost reduction, and use Level 3 neural for experiments.
See LevelThreeBenchmark.txt for timings, quality checks and their limits.

Experiment logs, generated training datasets, builds and screenshots remain
outside Git, under the ignored workspace scratch directory.

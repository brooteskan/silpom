Level 3 local relief visibility -- work-in-progress checkpoint, 2026-11-12

The SilPOM Mesh component's Local shadow method selects the original marcher,
a Gather marcher, or experimental neural visibility. Baseline remains the
default. Neural shadow confidence defaults to 0.995; higher values send more
rays to the Gather fallback. Camera intersections and external shadows use
their existing paths. Existing saved levels inherit Baseline.

The neural path tests four existing march positions, then evaluates a
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

This checkpoint includes trained coefficients and source integration. The
existing 16 standalone regression suites pass, and the new visibility shader
compiles for SM 6.2 and 6.7. Dedicated Level 3 CPU/GPU conformance, processed
O3DE shader assets, scene comparisons, and final performance/quality gates
remain unfinished. Leave the saved level on Baseline pending those checks.

Experiment logs, generated training datasets, builds and screenshots remain
outside Git, under the ignored workspace scratch directory.

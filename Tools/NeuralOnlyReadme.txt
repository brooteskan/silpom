Neural-only local visibility experiment -- 2026-11-12

This tests the existing four-probe / 10 -> 12 -> 1 network as a replacement
for the local shadow marcher. It does not qualify that network's quality for
production use. Baseline remains the default and the saved level is unchanged.

In VueLiveLink, select Irish Fieldstone Tower (Open Shaft), then SilPOM Mesh
and Local shadow method. The new Level 3 neural only option is distinct from
the retained Hybrid neural + march reference. Hybrid shadow confidence only
affects the hybrid. Neural-only uses fixed per-profile classification cutoffs
selected on independent calibration data, with no uncertainty continuation.

Compilation boundary:
Materials/Pipelines/MainPipeline/SilPOMHitForwardNeuralOnly.azsli defines
SILPOM_NEURAL_VISIBILITY_ONLY. It includes NeuralVisibility.azsli and shared
VisibilityInputs.azsli; it does not include LevelThree.azsli, and the original
SilPomReliefVisibility declaration is removed by preprocessing. A material
functor enables this shader and disables the historical forward shader when
method 3 is selected. The draw pass and camera-intersection shaders are shared.
The neural-only visibility source cannot call either local visibility marcher.

Input acquisition is bounded: one receiver-height quad and at most four probe
quads. A probe can prove shadow immediately. Otherwise the network's sigmoid
score is classified into exactly 0 or 1. This is binary local visibility, not
an area-light visibility fraction. There are no variable-length visibility
loops or retries. Analytic cases (disabled shadows, relief above its global
bound, straight-up light) retain their direct answers.
The sample-count control chooses the positions of those four probes in this
mode; reducing it does not reduce the probe budget. Zero disables local shadows.

Material preparation rejects unsupported neural-only configurations: the
method requires DX12 MainPipeline hit reuse, authored trained open-shaft
profiles 4-7 with their corresponding height assets and calibrated relief
depth 0.5, and
sample schedules of 0 (off) or 5-64. Changing the representation does not
silently select a marcher. Altered height content requires offline retraining.

The finite descriptor deliberately saturates travel beyond 256 texels and
normalized rise outside +/-2, then still makes a neural decision. These are
reported separately as saturated descriptors; they are not a runtime fallback
or a claim of trained-domain coverage. This experiment is not qualified for
those tails. The compact accuracy evidence records saturation counts.

Evaluation:
Tools/evaluate_visibility_only.py reuses the trained coefficients and disjoint
50,000-query calibration / 50,000-query test sets per profile. Both are
re-evaluated at relief depth 0.5, including the shader's physical-reach cap;
the existing network coefficients remain fixed for this initial experiment.
Thresholds
minimize calibration binary errors; ties prefer fewer false-lit errors.
The independent FP64 continuous bilinear-cell reference remains offline.
Reports separate neural-only, historical hybrid, baseline, MLP-evaluated,
sample-count and saturated/unsaturated groups. Binary metrics classify explicit
binary outputs; sigmoid uncertainty is never interpreted as soft visibility.

Held-out errors at the selected cutoffs (50,000 queries each):
                  cutoff         false lit   false shadow    total error
  Wall            0.599265933         800            228          2.056%
  Stairs          0.457549691         530             15          1.090%
  Trim            0.530893803        1011            231          2.484%
  Reveal          0.443724692         589             41          1.260%
Total: 3,445 / 200,000 wrong (1.7225%), versus baseline 966 (0.483%).
The current descriptor/network loses accuracy without the marcher's answers.
The stairs model is an offline test; level stairs remain ordinary geometry.

Next representation work should supply spatial information about the relief
between probes, for example a learned material-specific feature atlas with a
small decoder. More hidden units alone cannot resolve indistinguishable inputs.
The thin-blocker regression constructs two queries in one height texture with
identical ten-float descriptors but opposite independently checked visibility.
Do not use the hybrid's accuracy as evidence for standalone model quality.

Verification: 22 regression groups, including 9 neural-only API/structural
tests, CPU fixed-budget/threshold checks, and separately compiled SM 6.2/6.7
GPU parity tests. The preprocessing test proves both marcher symbols are absent
from the neural-only test shader. Each GPU run checks 49,152 neural-only queries.
NeuralOnlyEvaluation.json stores compact calibration and held-out evidence;
large datasets, logs and captures stay in ignored scratch directories.

The fair scene comparison uses depth 0.5 for every method. At nine reference
samples, Gather is faster than neural-only (0.466 vs 0.543 ms forward pass).
At 64, neural-only is faster (0.545 vs 1.092 ms). Its input budget remains fixed;
the sample setting changes probe placement, not the number of height tests.
The current weights still have appreciably worse standalone visibility quality.
Keep this as an experiment rather than promoting it as the production default.
See NeuralOnlyBenchmark.txt and NeuralOnlyBenchmark.json.

Run the GPU/scene comparison from the TG source project using
Tests/Run-VisibilityOnlyBenchmark.ps1. The versioned Python driver is
Tests/visibility_only_editor_benchmark.py; it validates unsupported settings,
prewarms all methods, captures matching cameras, and restores the original
configuration before exiting. Large evidence is under the ignored directory
scratch/silpom-neural-only-20261112/depth-05-comparison. The saved level remains
at depth 0.3 with Baseline selected.

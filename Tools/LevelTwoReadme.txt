SILPOM Level 2 experimental intersection predictor

VueLiveLink > Irish Fieldstone Tower (Open Shaft) > SilPOM Mesh:
  Level 2 predictor (experimental)

Off selects Level 1. On selects the new predictor for open-shaft models 4-7;
other profile models retain Level 1. Shader specialization removes the unused
path. It works with SM 6.2 and 6.7 and with either height format/Gather setting.

The new network is 19 -> 24 -> 5, with squared-ReLU hidden units. Inputs are
eight height-minus-ray residuals, eight triangle-edge proximities, two ray
travel rates and incidence. Outputs are two candidate fractions, a hit score,
and two estimates of correct refinement within two iterations. FP32 arithmetic
and the existing height map remain the geometric definition. No feature atlas
or duplicated source textures are introduced.

The higher-quality candidate is tried first. With at least one retry, the
second is also validated, even if the first succeeded. The nearer accepted
intersection wins. This improves first-hit selection but is still a heuristic,
not a complete traversal guarantee. A second seed uses one retry. Remaining
retries can run the baseline seed/recovery path after both learned seeds fail;
all learned attempts share the configured total cap. Low-confidence Level 2
predictions retain the full Level 1 opportunity. Grazing model-4 walls then run
bounded ordered nearest-hit recovery before flat fill, with either predictor.
Recovery has its own fixed work cap; it does not modify the saved candidate
budgets. See Docs/NeuralValidation.md for eligibility and remaining limitations.

This is a quality experiment, not an assumed performance improvement. It uses
eight descriptor probes instead of four and may run more corrections. Keep
Level 1 as the performance reference and compare at identical quality settings.

Tools/level_two.py trains and evaluates on descriptors captured from the shared
FP32 shader code. It calibrates candidate-quality heads against actual bounded
refinement; calibration freezes geometry/seed weights. Training, selection and
held-out rays have separate reproducible seeds. The runtime height maps are
used, including the stair map that differs from the authoring copy. The level's
stairs remain ordinary geometry.

Checkpoint selection prioritizes the no-regression gates, then quality/work.
Suspected teacher false negatives are independently checked by the FP64 cell
solver before correcting comparison labels. Reports retain every correction.
Depth P95/P99/max comparisons allow 0.01 mm or relative 1e-5 for FP32 rounding;
coverage, fill and false-positive counts retain strict integer comparisons.
Independent numerical checks are not certified interval proofs.

Build the standalone tools using Evaluate-LevelOne.ps1 or CMake. Then:
  python Tools/level_two.py <experiment.json> <Release-DLL-folder> <output>
Use --reuse-checkpoints to re-evaluate saved weights after solver/check changes.
Outputs are experiments; installation is a separate reviewed file change.

The approved conservative VueLiveLink preset is 6 iterations, 4 retries,
tolerance 1, maximum relief distance 0 (automatic pixel LOD). The previous
2/1/5 and 1 m cap are recorded under scratch/silpom-level-two-20261112/before.
Distance-fade and lighting settings are unchanged.

scripts/SilPOMLevelTwoAB/Run.ps1 captures yesterday's preset, conservative
Level 1, conservative Level 2, 4/1/1 Level 2, Level 2 with Gather, and a repeated
Level 1 baseline. It restores all controls/camera/CVars without saving the level.
Outputs go under ignored user/SilPOMLevelTwoAB. Close Editor/Asset Processor
before running it. GPU timestamps and matched camera PNGs accompany each case.

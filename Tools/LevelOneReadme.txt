SILPOM Level 1 solver evaluation and checkpoint selection

Run Tools/Evaluate-LevelOne.ps1 from PowerShell. It builds standalone CPU tools
without rebuilding or launching the Editor. Defaults evaluate active tower
profiles 1, 3 and 4; profile 2 is the stairs, currently ordinary geometry.
Use -Profiles 1,2,3,4 to include it. Results go under ignored TGProject/user/
SilPOMLevelOne. Add -Train to generate candidate weights and -RunTests to run
the CPU/GPU regression suite. -PythonExe can override automatic O3DE discovery.

The evaluator runs the shared shader's prediction, refinement, retries and
grazing fill code. Optional hooks count iterations, candidates, first/retry
successes, confidence rejections, fills and rejection reasons. Hooks disappear
from ordinary shaders; there are no runtime counters or extra shader resources.
CPU descriptor filtering uses ideal FP32 bilinear sampling. Hardware filter
precision and floating-point contraction can differ; CPU statistics are not
pixel-weighted scene measurements or GPU timing predictions.

Compare budgets 1/1/5, 2/1/5 (current level), 4/1/1 and 6/4/1 (component defaults).
Each report contains outcome counts, per-ray iteration/candidate histograms,
depth comparisons, source hashes, reproducible seeds and independent audits.
Depth disagreements use an explicit 0.25 mm threshold; they are not all proven
wrong roots. The existing teacher is itself an approximate CPU solver.

Independent FP64 checks parameterize the full affine displacement surface by
height. UV and ray distance become rational polynomials. Texel boundaries,
triangle edges and ray interval boundaries partition the problem into bilinear
cells; each cell gives a polynomial of degree at most five. Its roots are
checked against the surface, then sorted by ray distance. This does not reuse
the teacher's clipping or Newton algorithm. It is a numerical audit, not a
certified interval proof. Singular/ill-conditioned checks are unresolved.
Audits deliberately include large depth disagreements, false positives, fills,
retries, misses and grazing rays. Saved ray/face data allows case replay.

Training keeps the 11 -> 16 -> 2 network and uses three distinct random seeds
for training, checkpoint selection and final held-out evaluation. Failed,
filled and depth-disagreeing examples receive three times the sampling weight. Checkpoints
at 800, 1600, 2500 and 3300 steps are evaluated through the actual solver.
Selection requires no increases in uncovered teacher hits, teacher hits using
fill, refined false positives, depth disagreements or depth-error P95/P99/max.
Depth-error tails allow 0.01 mm or relative 1e-5 for FP32 rounding; integer
coverage/error counts retain strict comparisons. Among eligible models, quality
counts, iteration work and retries determine ranking. Final held-out
regressions reject a candidate. Raw checkpoints remain available for review.

Candidates are experiments. No automatic promotion or shipping-weight changes
occur. A selected candidate still needs independent audit and GPU/visual tests
at matched quality. Synthetic rays deliberately overrepresent difficult grazing
and edge cases; their miss percentages do not describe visible screen coverage.

For direct Python use:
  python Tools/level_one.py <experiment.json> <Release-DLL-folder> <output>
      --profiles 1 3 4 --rays 4000 --audit-rays 12
Add --train for checkpoint selection. --welded selects the older three-profile
tower. Output under the gem's shipping Assets directory is rejected.

# Neural validation controls

## Strict neural lighting and receiver reuse

`NeuralOnly` applies to the enabled generation, including the interval while a
replacement is loading or validating. A previous generation remains visible
only when its actual draw-packet products, shader variants and options certify
the strict contract. Incompatible packets are suppressed; missing assets or
validation failures never enable a baseline/hybrid replacement. Publication and
packet/shader updates recheck capability. Unsupported estimator/profile
combinations are rejected. Explicit directional/angular comparisons require the
trained wall profile; the physical neural estimator also supports the existing
stairs, trim and reveal profiles.

A final end-of-render-preparation check also handles Atom's packet rebuilds
that bypass its packet-update notification. Suppression must cover items already
collected into draw lists: disabled flags alone are insufficient at that point,
so suppressed owned draws carry zero instances. Motion items remain suppressed
independently when a valid generation's other draws are restored.

Scene-notification lifetime follows requested strict policy, active/pending
suppression or restoration, and trained-rendering duties. The subscription is
reconciled before validation/loading can return and retried when a scene becomes
available. Publishing a nontrained generation no longer unconditionally
disconnects it. Strict-only duties keep the final suppression callback alive
without installing trained rendering passes.

Lighting uses `SILPOM_NEURAL_VISIBILITY_ONLY`: enabled queries select the supported
neural estimator, retain its analytic exits and bounded input acquisition, and
never recover from uncertainty, saturation or a missing receiver cache by
marching. The physical estimator retains up to four light-specific off-center
height probes and its final neural decision. An unavailable cache evaluates the
same estimator uncached. Shared, directional/angular and proxy-appearance
lighting do not acquire physical-ray height probes. A pipeline-wide traversal
guarantee additionally requires the certified trained depth products using
`SILPOM_NEURAL_NO_MARCH`; lighting alone cannot certify an ordinary depth shader.
Bounded trained-hit refinement and existing repair/proxy recovery are separate.
`cacheAvailable` is normally true for these register-local receiver contexts;
the uncached branches are explicit hooks exercised by tests, not runtime
cache-allocation failure detection.

Direct lighting creates one local receiver context after the invalid-hit guard,
after normal-only AA probes have finished, center UV has been restored and full
center material evaluation has published lighting state. Each MSAA invocation
owns its context. Repaired hits and LOD shells use their final recorded owner,
representation, UV and gradients. The BRDF continues to use the center surface
after decals; probe coarse flags retain only their AA role.

Preparation follows the selected backend:

- Physical neural queries first check enablement/scale, then lazily prepare the
  center displacement frame and determinant. Per-light rate/direction exits run
  before acquiring starting height with the original shadow-height recipe.
  Qualifying later lights reuse that height; reach, schedule, probe positions
  and values remain per light. Normal and generic height helpers cannot seed it.
- Shared visibility reuses its query frame, face metrics, projected displacement
  direction, center UV and center shared material state. Directional/angular
  comparisons share their query frame and retain the hidden layer/angular state
  already decoded by full center evaluation.
- Proxy appearance reuses full center features, footprint, view and frame. Its
  light head remains the joint visibility/reflectance correction for diffuse
  and specular response. Current routing selects its original noncomparison
  shader; a directional visibility estimate does not replace this correction.

Shared visibility caches 12 first-layer preactivations through inputs 0–7;
proxy appearance caches 16 through inputs 0–10. Prefixes include the bias and
are prepared after enablement and hemisphere gates. Each light continues the
original suffix accumulation before ReLU and output evaluation. Receiver-only
ReLU values and final visibility are never cached. Both spiral rays receive the
same center context as directional lighting.

Focused validation covers lazy height acquisition and exact CPU ray arithmetic,
cached/uncached neural decisions, unavailable-cache behavior, independent sample
ownership, transitive no-march shader boundaries, and AA state isolation.
GPU prefix/reuse checks include SM6.2/SM6.7 and trained bundles; the observed
cached/uncached output difference is at most `1.19209e-7` with default DXC
optimization. Current-source material variants are compiled through MCPP,
AZSLc and DXC separately from the asset cache. These checks do not establish
combined application image equivalence or a speedup. No timing measurements
were collected; the extra receiver register lifetime remains a tradeoff.

## Height acquisition in bounded validation

Each `SilPomRefineFace` call starts an empty cache containing an unaddressed
integer cell and its four raw height texels. Iterations in the same cell reuse
those texels; a changed cell reloads the complete quad. Current UV fractions,
bilinear coefficients, height and derivatives retain their original expressions.
Gather uses the existing interior footprint and channel order, with the original
integer-load fallback at borders and one-texel dimensions. Candidates, retries,
faces and MSAA invocations never share this cache. Lighting and normal helpers
do not participate in its lifetime.

Iteration limits, tolerances, damping, containment, ray intervals, candidate
ordering and nearest-hit recovery remain unchanged. The independent face
direction is evaluated before the cached height lookup to preserve the approved
DXC tangent-sum association on the stored-coefficient path. GPU regressions
cover that ordering; no precision mode or tolerance was changed to hide a
rounding difference.

Exact NPOT repeat addressing now defaults on through `SILPOM_FAST_ADDRESSING`,
independently of MSAA. Clamp and power-of-two masking remain unchanged. In-range
NPOT coordinates return directly; wrapped coordinates use one signed remainder
and add the dimension only for a negative remainder. The existing constant
divisors remain available. `SILPOM_GENERIC_REMAINDER=1` selects general remainder
and `SILPOM_FAST_ADDRESSING=0` retains the double-modulo reference for tests.
Coordinate construction, neighbor selection, shader products and supervariant
names are unchanged.

The legacy bounded traversal now tests blocks before acquiring a leaf quad.
It retains adjacent-cell texel reuse after a successful leaf, invalidates the
quad after block skips, and reloads only when a later leaf needs evaluation.
Its DDA, root ordering and cell-budget accounting are unchanged. Strict trained
rendering compiles out this generic traversal, so this last change has no direct
strict-mode benefit.

Focused checks passed:

| Check | Scope |
| --- | --- |
| Newton CPU equivalence | 16,000 frozen/current solves with exact status, UV, ray time and diagnostics. Fixture height loads decreased from 116,596 to 68,520. Separate repaired-owner, texture, sample and retry calls start cold. |
| Newton GPU equivalence | 3,145,728 frozen/current comparisons across Load/Gather, R32/R16 textures, repeat/clamp, stored/inline coefficient paths and SM6.2/SM6.7. Height, derivatives, solver results and diagnostics match bit for bit in these fixtures. |
| Stored coefficient regression | Ten GPU runs cover cached, missing, disabled, Newton-fallback and area-fallback configurations on both shader models. Each run checks 8,704 Newton and 8,720 gradient results, including 128 frozen wall faces, with zero mismatches. |
| Legacy traversal | 149,407 CPU rays match every hit field and cell count against the frozen eager-load reference; 8,192 GPU differential rays also pass. CPU fixture leaf loads decreased from 707,666 to 617,402, and fully rejected blocks acquire no height quad. |
| Addressing | Four standalone addressing policies each cover 2.4 million signed coordinates against a 64-bit floor-division oracle, including seams, repeated negative wraps, extreme integers and one-texel dimensions. |
| Integration boundaries | 18 related CPU CTest suites, 17 strict/AA/lifecycle checks, strict native C++ compilation and seven current-source material compilations pass, including default-MSAA and NoMSAA depth products. |

The compiled checks cover these fixtures, not complete application image
equivalence. Source-only analysis did not establish the numerical result: the
stored-coefficient GPU oracle caught a one-ULP association change, which the
independent direction/height ordering fixes. No timing measurements or speedup
claim are attached to the fetch-count reductions. Reproduction artifacts are
under `scratch/point-five-20261117`, `scratch/newton-height-cell-20261117` and
`scratch/heightfield-fetches-20261117`.

## Candidate solve controls

Select a **SilPOM Mesh** entity in the level and adjust these component settings:

| Control | Default baseline | Lower-cost comparison | Effect |
| --- | --- | --- | --- |
| Validation iterations (lower = cheaper) | 6 | 4 | Maximum Newton iterations in each no-march candidate solve; range 1–16. |
| Validation retry cap (lower = cheaper) | 4 | 1 | Caps retries after the initial solve; range 0–4. Zero disables retries. Existing eligibility rules still apply, including the wall models' one-retry limit. |
| Validation tolerance (higher may be cheaper) | 1 | 1 initially | Multiplies the positional convergence tolerance; range 0.01–100. Higher values may terminate earlier, but accept larger positional errors. |

These defaults configure the learned candidate stage, including its conditional retry counts and convergence tolerance. Existing levels that omit these fields inherit those defaults. Grazing open-shaft walls additionally use the bounded recovery described below. To compare candidate budgets, use the same entity and camera with **6 / 4 / 1** and **4 / 1 / 1**. No shader variant change is required when editing these settings after the updated editor and assets are built.

These controls affect the newer no-march learned candidate and its retries. They do not change the legacy neural/cache prepass, generic heightfield traversal, confidence cutoffs, triangle containment checks, or backing geometry. Fewer iterations or retries can increase fallback coverage or leave holes where no backing surface is available. A larger tolerance does not guarantee a speedup.

The component forwards settings to material properties `surface.validationIterations`, `surface.validationRetryCap`, and `surface.validationToleranceMultiplier`. Values outside the supported ranges reject the component configuration, including nonfinite tolerance values.

Compare GPU timings and visual quality during camera movement, especially at triangle edges, silhouettes, openings, and grazing angles. Baseline algorithm preservation does not imply identical GPU cost to the former compiled shader: the new limits are runtime material inputs.

## Grazing wall nearest-hit recovery

Open-shaft wall model 4 runs a compact ordered recovery search when
`abs(dot(N, viewDirection)) < 0.30`. Other profiles, face-on rays and distant
Level 5 raster proxies skip it. This applies after either learned predictor,
before the existing flat-wall/hole-fill policy. No component settings change.

The average displacement direction supplies a projected seed ray. Filtered
mip-zero height probes run from front to back, normally at half-texel spacing,
with at most 128 intervals (129 samples). Sign crossings or a near-zero residual
supply secant seeds. Up to four candidates receive eight iterations of the
original affine-direction validator with tolerance multiplier 1. Geometry is
accepted only inside the actual triangle and ray interval, at or before an
existing accepted hit. Rejected/exhausted recovery retains the original result;
it never publishes a projected seed or unconverged iterate as a surface.

This is bounded recovery, not a complete nearest-root proof. The interval cap
can widen probe spacing; narrow double crossings, tangent contacts and strongly
deformed seed parameterizations can still be missed. The learned candidate
budgets and all saved quality/lighting/LOD settings retain their values.
`SilPOM.NearestHit` covers multiple roots, rejected predictions, affine surface
validation, small camera motion, signed tiling, repeat/clamp and fixed work
bounds. GPU tests separately compare recovery geometry on striped fixtures;
hardware-filter weight precision need not produce identical Newton counts.

Level 5 also supports disabling height-derived normals on the near path. Proxy
appearance still decodes its learned normal independently; the switch controls
near-surface height-gradient shading, not geometry validation or proxy coverage.

### Approved visual reference and scheduling optimization

The approved 2026-11-13 scalar recovery is frozen in
`Tests/Reference/NearestHit.azsli`. The production path fetches four independent
height probes together and builds a candidate mask. It validates candidates in
the original order, clearing the lowest set bit first. Probe timestamps use the
original FP32 expressions and are recreated only for candidates, reducing live
registers. The validator has one loop body instead of per-probe unrolled copies.
Probe locations, eligibility, spacing, cap, refinement limits and saved settings
are unchanged. Prefetch may read up to three additional probes before an early
return, but never goes beyond the original 129-probe limit.

`SilPOM.NearestHit.Equivalence` checks 6,000 queries bit-for-bit against the frozen
scalar implementation, including validation decisions and bounded speculative
reads. The additional held-out comparison covers 13,844 real wall rays with each
predictor. These checks supplement the existing independent surface tests.

`Tests/Reference/NearestHitVisual.json` records the approved camera poses, tower
controls and hashes for 13 static/motion PNGs and seven hit-buffer readbacks.
Run `python Tests/NearestHitVisualTests.py <capture-directory>` on a capture with
`results.json` and `current/` outputs. RGBA pixels, hit UVs, depth, representation
flags and status must match exactly. Only per-launch ownership allocations are
normalized away. The whole-level hash records provenance; unrelated entities may
be edited without changing the tower's visual reference.

## Additional performance controls

The **SilPOM Mesh** component also exposes these runtime settings. The defaults
retain the prior rendering behavior; no existing level values need changing.

| Control | Default | Cheaper direction and effect |
| --- | --- | --- |
| Relief LOD pixel budget | 0: inherit current mode (4 for this tower) | Higher positive values switch to ordinary shell geometry sooner. Uses the existing 656-line reference, not the current viewport height. |
| Maximum relief distance | 0: automatic LOD only | A smaller positive value caps the near relief draw at that view depth in metres, including its overlap with the shell. Applies to LOD modes. |
| Validation distance fade start / end | 0 / 0: disabled | Set end above start to blend toward the distant validation settings. Distances are from the camera to each face origin in world metres. |
| Distant validation iterations | 4 | Lower caps local solver iterations farther away. Integer budgets are interpolated and rounded over the fade interval. |
| Distant validation retry cap | 1 | Lower reduces distant retries; zero disables retries. Existing eligibility and wall limits still apply. |
| Distant validation tolerance | 1 | Higher can accept close solutions sooner, with larger positional errors. |
| Spiral-light shadow samples | 18 per ray | Lower reduces shadow samples; zero disables this local occlusion while keeping the light. Only affects materials receiving spiral light. |
| Height-derived normals | Enabled | Disable to skip the displacement-gradient normal calculation. Material normal maps remain; intersection validation still reads heights. |

The LOD controls keep the current winning-hit resolve mode and its backing
geometry. They do not change the material's special Maximum cells value. Small
distance caps shrink the near/shell overlap to avoid drawing relief beyond the
cap. When distance validation is disabled, the distant limits are ignored.

For an initial comparison, try pixel budget **8**, leave distance capping at
**0**, and retain the current validation tolerance that looks good. Then test
distance validation separately, for example fade **10–30 m**, distant iterations
**4**, retry cap **1**, and a distant tolerance chosen for the scene. These are
comparison settings, not measured performance recommendations.

## Spiral light enable/disable

The **Spiral light** dropdown applies only to displaced SILPOM geometry:

- **Off (cheaper)** disables both the first window and every additional spiral
  window, retaining their settings. It also skips allocation of their window buffer.
- **Selected materials** is the default and preserves the previous per-material
  **Receive spiral light** behavior.
- **All SILPOM surfaces** enables spiral light on all displaced batches, regardless
  of their per-material receive flags. Ordinary batches, including the current
  base-mesh stairs, keep their existing material and lighting.

Status reports the number of relief batches receiving spiral light, or confirms
that it is off. The current tower has zero eligible receivers in Selected
materials mode because only its ordinary stair materials opted in. All SILPOM
surfaces enables receivers on its heightmapped masonry instead.

Peak lux controls only the first window, rather than acting as a master gain.
Each additional spiral window has its own lux value. Their contribution still
depends on the configured spiral path, distance fade and surface orientation.

### Master brightness and top fade

**Spiral light master brightness** scales the first window and every additional
window together: **1** preserves their authored lux, **2** doubles the resulting
light, and **0** mutes all spiral lighting and its local shadow work. This does
not change ordinary stair materials or the individual windows' saved lux values.

**Spiral light top fade** exposes the previously hidden height envelope:

- **X** is the local end height in metres, where the contribution reaches zero.
- **Y** is the distance below X over which it fades smoothly. Zero disables the
  height limit. Larger positive values give a broader, softer fade.

The tower previously ended this contribution at **20.01 m** with only a
**0.35 m** fade, even though its highest spiral window is at **22.43 m**.
The corrected tower preset ends at the **24 m** masonry top and fades over
**2 m**. Other meshes retain their existing end settings. Brightness is applied
after the smooth envelope, so it remains a linear multiplier at every height.

Spiral illumination also blends the two adjacent staircase turns. The previous
nearest-turn implementation faded every turn to zero before switching turns,
which created an artificial dark helix on walls. The replacement blends their
light contributions and ray bias continuously. It retains the two-direction
shadow limit and one buffer fetch per additional window. Real local relief
shadows and finite window reach remain enabled.

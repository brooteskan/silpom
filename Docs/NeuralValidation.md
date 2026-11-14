# Neural validation controls

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

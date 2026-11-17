# Normal-query setup refinement

The four owner-local learned-material and proxy normal probes now share setup
that is invariant across the stencil. One complete center surface evaluation
still runs last and publishes the center material, appearance and lighting
state. Hit records, depth and gradient sidecars are unchanged by this refinement.

## Implementation

- Learned materials prepare LOD, tap count, up to eight tap offsets and four
  footprint context values once. Each shifted UV still samples the two feature
  textures and evaluates the normal decoder. Tap expressions retain their
  original operation order.
- Proxy probes prepare the owner plane, projected tangents, metric, tangent
  basis and world-to-local transform once. Each probe receives position and
  front-face state directly; it restores its own authored-plane query,
  conditioning, footprint, filtered features and view. These local queries
  preserve the center appearance globals and return their own coarse flag.
- Probe work requires specular AA, a valid lit sample and conditioned source
  gradients. Disabling material normal maps still permits the geometric-normal
  stencil; learned normal feature sampling/setup is skipped in that case.
  Disabling specular AA skips setup and probes and adds no AA roughness.

The implementation is in `LearnedNormalFootprint.azsli`,
`LearnedMaterialSurface.azsli`, `ProxyAppearance.azsli`,
`NormalQueryPolicy.azsli` and the material's `SilPOMMesh.azsli` integration.

## Intentional coarse-probe policy

The AA hook selects its coarse policy if the source context is coarse/invalid,
the center proxy appearance query is coarse, or **any** normal probe is coarse.
Nonfinite normal differences also select that policy without modifying the
source context's validity. A finite normal returned by an unconditioned probe
does not establish a usable normal differential.

The coarse branch adds the existing maximum contribution of `0.18` to
`roughnessA2`, saturated to `[0,1]`. Conditioned queries retain Atom's existing
variance scale and clamp. Propagating a coarse offset is an intentional boundary
behavior change from a center-only decision; prepared setup and acquisition are
otherwise expected to preserve the preceding normal-only query results.

## Focused checks

All GPU proofs below passed on AMD Radeon(TM) Graphics.

| Check | Scope and result |
| --- | --- |
| Learned prepared footprint | SM6.2, DXC `-O3`; 4,096 inputs × four probes for each of three weight sets: deterministic random, `wall.l4mat`, and `wall-shared.l4shared`. Two feature textures with seven mips cover tap counts 1–8. Per run, 67,692 tap UV pairs, 196,608 feature/context float components and 32,768 decoded normal output components match the unchanged acquisition path bit-exactly. |
| Proxy prepared setup | SM6.2 and SM6.7, three weight sets each: deterministic random, `wall.l4mat`, and `wall-appearance.l5app`. Six runs total 98,304 probes, 1,769,472 decoder-input component comparisons and 294,912 normal-component comparisons, all bit-exact against the unchanged full-input/full-decoder path. Each run includes 3,113 coarse and 13,271 valid probes. Coarse flags and center globals also match; all normals are finite. The separate full-decoder CPU/GPU oracle error is at most `1.78814e-7`. |
| Normal-query policy | 41 CPU checks and three passing standalone CTest entries: CPU plus SM6.2/SM6.7. Each GPU profile checks 262,144 float/metadata outputs across 8,192 contexts with embedded policy assertions. A real projected plane has a valid center footprint of `0.8` source UV/pixel and a coarse quarter-pixel offset with an unbounded footprint of `3.2`. Finite constant probe normals still force maximum AA (`0.2 → 0.38` in `roughnessA2`). AA-off and geometric-normal-only gating are covered. |

The proxy fixture copies the current helper bodies and substitutes deterministic
UV-and-LOD-dependent values for feature texture `SampleLevel` calls. It proves
the prepared control/input/decoder behavior within that fixture; it does not
establish production texture sampling or full-material image equivalence.
The learned proof uses actual feature textures. These isolated proofs do not
replace final application validation of the combined changes and coarse policy.

Workspace evidence: `scratch/learned-prepared-footprint/proof.json`,
`scratch/proxy-probe-refinement/proxy-prepared-proof.json`, and
`scratch/silpom-gradient-math-20261116/normal-query-policy-report.json` contain
the detailed scope, outputs and source/artifact hashes.

## Runtime validation

The combined change passed all five fixed 1280x720 views at 2x MSAA with
height-derived normals enabled and AngularEdges4 lighting (mode 5). All 35
full-resolve attachment comparisons against the corrected full-surface-query
baseline were bit-exact across 5,557,447 owned shaded samples. An independent
direct comparison also passed all 35 attachments against the immediately
preceding normal-only build, with authenticated baseline hashes. All 70 current
full/rectangle/repeated-full attachment comparisons were also exact. Stored hit,
depth and gradient samples remain unchanged, including all 7,372,800 hit/depth
samples in the four original benchmark views. No changed AA samples were
observed in these views; the dedicated policy fixture exercises the intentional
coarse-probe boundary change.

The wider view includes 2,563 authored-plane proxy samples. These captures did
not select morph, learned proxy depth, diagnostic or copied-donor repairs;
compile/math coverage remains separate from runtime coverage. All 79 asset jobs
completed, readiness found no failed/stale shader products, and all 262
controlled source/fixture/harness/binary hashes remained unchanged. The capture
Editor exited successfully after restoring temporary settings and leaving the
saved level unchanged. Per-sample inspection was enabled for readback, so the
capture's displayed frame times are not performance measurements.

The current-source compile proof covers all four forward families with AA
compiled in/out, with zero implicit derivative or texture operations. Scratch
instrumentation verifies zero probes and no counter UAV with AA off, and four
probes with AA on. The three NoMSAA bake variants and UV usage variant compile.
Production shaders contain no instrumentation.

Results: `user/silpom-gradients-20261116/prepared-normal-quality/refinement-report.json`
and `Tests/Reference/NormalQuerySetupRefinement20261116.json`.

No performance measurement is claimed here. The preceding benchmark predates
this refinement and does not measure its cost or speedup.

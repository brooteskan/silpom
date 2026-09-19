# Curved intersection coverage gate

This continues the experimental solver from `7a9f85f`. It does not select a
cooked traversal format or enable production mesh assets/rendering.

## CPU certificates

Regular roots now use Krawczyk interval inclusion before reaching a tiny leaf.
Every arithmetic operation in the fixed-cell polynomial and its derivatives is
rounded outward. A padded box covers the whole child triangle, including its
artificial subdivision edges. Strict inclusion establishes a root; a contraction
bound establishes uniqueness over that box. The contracted enclosure must lie
inside the actual source triangle and texel cell, and inside the finite ray
interval. Its depth and parameter widths must meet their separate budgets.
Exclusion is also permitted when the Krawczyk enclosure is disjoint from the box.

A separate, restricted exact path handles affine height with one affine projected
constraint. Eliminating that constraint gives a quadratic. An exactly negative
discriminant excludes all roots, and an exactly zero discriminant certifies the
isolated double root. Linear reduced equations are supported too. Triangle,
texel-cell, and finite-ray membership are checked with exact rational signs,
including closed endpoints. Floating-point depth endpoints are checked against
the exact rational value before being returned. A small residual or determinant
alone cannot accept a singular root.

The candidate and resultant/Sturm oracle share only the arbitrary-size integer
and dyadic arithmetic primitives in `CurvedDyadic.h`. Their polynomial formation
and root-solving algorithms remain separate. The oracle now performs corner and
height differences in exact arithmetic as well, avoiding binary64 rounding
before conversion. Its coordinate pairing and public attributes remain numerical;
it is not a formal oracle for unrestricted degenerate systems.

## Regression evidence

- All 1,024 constructed CPU grazing rays resolve as hits. The eleven former
  failures (148, 150, 152, 154, 156, 158, 171, 173, 175, 177, 960) have interval
  certificates and explicit comparisons against the exact reference for nearest
  depth, barycentrics, primitive, UV, and normal. Reversed candidate order agrees.
- The exact-dyadic tangent resolves with fewer than 32 nodes and a depth enclosure
  within the requested `2e-12` total width. Four signed offset magnitudes around
  the tangent distinguish misses from two separate roots, including the nearer
  root. Closed and immediately excluded finite-ray endpoints are tested.
- 348 boundary rays cover full shared/exterior edges, vertices, and texel
  boundaries on affine displaced fixtures, with signed displacement, three base
  scales, and reversed ownership order. General cubic seams remain a separate
  required extension; these affine tests do not establish that coverage.
- Seven deliberately restricted node budgets are checked on each of the eleven
  grazing regressions. They may return a proved hit or `Exhausted`, never a false
  miss. A zero-budget tangent remains `Exhausted`. Invalid heights and non-finite
  requested error budgets are rejected before exact arithmetic.

On the local Release build, the CPU grazing traversal takes about 1.15 seconds
for 1,024 rays, with 10.68 mean nodes, p95 29, and p99 45. Reference construction
and attribute checks are outside this timed section. The earlier baseline was
about 1.31 seconds, 131 mean nodes, p95 194, p99 497, with eleven exhaustions.
Fewer nodes do not translate proportionally into less time: outward interval
arithmetic has a substantial cost. These are research workloads.

## Error budgets and remaining limits

Depth is an enclosure in ray-parameter units; convert it to world distance using
the ray direction length. CPU regular inclusion also limits the parameter-box
width independently (`parameterTolerance`, normally `1e-8`). The grazing
reference comparisons separately allow `2e-11` for numerical oracle depth
evaluation, `2e-8` for barycentrics/UV, and `2e-12` for the normal dot-product
deficit. Those reference allowances never enlarge the returned hit interval.
Texture values are supplied as binary64 on CPU and transported as binary32 on
GPU; arbitrary texture decoding error is not covered by a depth tolerance.

## GPU certificates and measurements

`CurvedGpuInterval.hlsli` evaluates the transported fixed-cell polynomial and
derivatives with outward-rounded binary32 interval arithmetic, including
flush-to-zero. It applies the same inclusion/contraction approach as the CPU,
with an independent `5e-5` parameter width limit and the requested `5e-4` depth
budget. This resolves 27 of the original 28 failures without increasing budgets.

`CurvedGpuRational.hlsli` supplies a separate compute fallback for unresolved
precision leaves. It uses checked 32-bit integer mantissas and binary exponents;
overflow or unsupported algebra declines the proof. Common odd factors are
removed before forming discriminants and evaluating rational coordinates.
Linear and double roots are classified exactly. Distinct quadratic roots are
also supported when the eliminated coordinate is constant: outward enclosures
for both roots must establish domain and finite-ray membership and distinguish
their depths. The square-root instruction is only a locator; outward square
bounds verify its enclosure. Constant texel-boundary expressions are checked
exactly, rather than admitted by an epsilon.

The fallback re-audits every unsupported cell that could compromise the newly
proved nearest hit, with conservative subdivision where whole-cell bounds are
too broad. It shares the original node budget and preserves the previous
diagnostic when proof is incomplete. It does not run after explicit node, depth,
or stack exhaustion. A UAV barrier orders the two dispatches; both are included
in compact GPU timing. Keeping the integer path separate avoids increasing the
main traversal kernel's register pressure.

The original 288-ray GPU corpus now requires exactly 237 hits, 48 misses, three
invalid inputs, and **zero exhaustions**. Previously it allowed 28 exhaustions.
All four signed/scaled cubic groups require 48/48 hits. An additional 21-ray
silhouette sweep checks twelve exact contacts, six nearby offsets, and three
finite-interval cases. Reversed candidate order, insufficient main work budgets,
and an insufficient exact-fallback budget remain mandatory. The original planar
compute and procedural DXR tests each retain all 8,192 comparisons.

The shared bounded arithmetic source is compiled as both HLSL and C++.
Its accepted operations on 2,048 deterministic random binary32 input pairs are
compared against arbitrary-size dyadics, alongside explicit overflow, excessive
exponent alignment, invalid propagation, and common-factor tests.

Representative warm RX 7900 XTX medians after the change are about **1.9 ms for
192 front rays** and **16 ms for 25 grazing rays**, compared with approximately
15 ms and 47 ms at `7a9f85f`. Five timestamp runs follow a warmup; fine-mode
grazing takes about 26 ms. These workloads remain far too small to establish
frame-scale performance, and no cooked format is selected.
The final validation counts, settings, memory sizes, and timestamps are captured
in [CurvedCoverageMeasurements.json](CurvedCoverageMeasurements.json).

General singular cubic roots, non-affine shared-edge ownership, broader rendered
coverage, and realistic traversal cost still gate runtime promotion. The existing legacy
regular-leaf and boundary logic remains experimental. The new certificates do
not constitute a proof for every path in the prototype.

Reproduce with the standalone build commands in
[CurvedTraversalExperiments.md](CurvedTraversalExperiments.md). The CPU executable
also accepts `--grazing` and `--tangent` to run those regression groups alone.

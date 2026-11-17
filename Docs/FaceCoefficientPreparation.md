# Immutable face coefficients

`FaceCoefficients.azsli` prepares a separate 48-byte record from the final
canonical 144-byte face values. The record contains `D_u.xyz` and raw signed UV
area, `D_v.xyz` and base Newton tolerance, then four metadata words. The first
metadata word holds ready/area/Newton/gradient flags; the second is version 1.
The canonical descriptor layout is unchanged.

Preparation retains the original affine, unnormalized displacement partials
and `2e-6 * max(1, max(length(du), length(dv)))` tolerance prefix. The requested
tolerance multiplier remains at the Newton call site. Gradient validity keeps
its existing finite-value and `abs(area) >= 1e-20` tests; these are not new
Newton rejection criteria. Missing, unsupported or unusable coefficient data
executes the original calculations. Cache-enabled shader faces carry their own
attached record, so nested face copies cannot accidentally use another face's
coefficients. Other shader variants and existing CPU adapters retain the
uncached layout and path.

The raw area is available through the conditional `SilPomUvArea` cache lookup,
but `SilPomFaceWeights` retains the original uncached denominator expression.
A GPU test found that a conditional cached-area load in that barycentric path changed
reciprocal common-subexpression handling inside Newton: 82 comparisons changed
UV/depth by one ULP despite identical stored area bits. Keeping the original
barycentric expression graph eliminated those differences while retaining the
cached Newton partials, tolerance and explicit-gradient partials. No precise
qualifiers, compiler math-mode changes, cached reciprocal, normalized direction
or rewritten displacement interpolation were introduced.

Newton retains one loop with cached or fallback setup selected before it. The
gradient wrapper likewise selects cached partials or its original fallback
arithmetic. These are the first implementation's restored semantics. The later
whole-function fallback experiment is preserved as a rejected regression
artifact; it changed additional runtime hit/depth samples and is not the current
implementation.

## Focused verification

The CPU test passed 82,801 checks over 2,183 canonical packed inputs, including
128 frozen export-derived tower records, 6,528 complete Newton comparisons and
2,180 gradient comparisons. Fixtures include mirrored UVs, zero/tiny signed
areas, distinct Newton/gradient validity, missing records and wrong versions.

On AMD Radeon(TM) Graphics, SM6.2 and SM6.7 with DXC `-O3` each passed cached,
all-records-missing and cache-disabled modes. Each run used two dispatches with
a real UAV-to-SRV dependency: 8,732 stored-versus-inline cases, 8,704 complete
Newton comparisons (8,684 hits) and 8,720 gradient comparisons, with zero
mismatches. Finite values are compared bitwise; inactive NaNs in degenerate
coefficient payloads are compared by NaN classification. The frozen legacy
consumer expressions are separate from the new preparation helper.

`Tests/Reference/FaceCoefficientTowerPacked.bin` is an export-derived FP32
canonical-record fixture, not a live GPU descriptor-buffer dump. Its source
identity and packing policy are in the adjacent JSON file. Both test programs
also accept arbitrary canonical 144-byte record files. The detailed outputs,
source/binary/shader hashes and fixture limitation are recorded in
`scratch/face-cache-20261117/proof.json`. Restoration checks and fresh test
artifacts are recorded separately in `proof-restored-initial.json`; the first
proof and rejected experiment remain unchanged.

## Runtime comparison policy

Coverage, owner/status, recorded depth, shading, and gradient representation,
repair provenance and validity must match exactly. Finite source UV coordinates
and float derivative components may differ by at most two ULPs. This is an
explicit numeric tolerance; it is not bit-exact equivalence of the complete hit
and gradient attachments.

The initial application comparison found one lower-glancing source-U difference
of two ULPs, with unchanged depth, ownership, status and shading. A fresh five-view
validation of the restored source under the stated policy is pending. Cache
publication and ordering also require application validation. No performance
result is claimed by these arithmetic conformance tests.

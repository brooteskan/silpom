# Curved intersection feasibility prototype

This prototype is deliberately separate from the SilPOM runtime and cooked asset
ABI. It tests whether the mesh surface and nearest-hit rules can be implemented
without weakening the existing planar contract. None of the types in
`Tests/Curved` are serialized or exposed by the Gem.

## Surface contract

For barycentric coordinates `b`, the rigid-frame surface is

```
S(b) = s P(b) + a (H(UV(b)) - r) D(b)
```

`P`, `UV`, and `D` are affine. Corner directions are unit length, while the
interpolated direction is intentionally not normalized. `s` is positive uniform
base-mesh scale and `a` is a displacement coefficient in metres; entity scale
does not multiply displacement. Height is linear scalar, mip zero, explicitly
bilinear at texel centres, with repeat or clamp addressing.

The implemented geometric derivatives are

```
Su = s Pu + hu D + h Du
Sv = s Pv + hv D + h Dv
```

and the oriented geometric normal is `normalize(cross(Su,Sv))`. A degenerate
normal cannot produce `Hit`.

## Algorithm under test

The CPU prototype clips each source triangle against the height texture's texel
cell lattice. Within a cell, bilinear height composed with affine UV is quadratic;
multiplying it by affine direction makes the surface at most cubic. Each clipped
fragment is represented as a degree-three triangular Bernstein patch.

For each ray, two components perpendicular to its dominant direction form a
bivariate root problem. Bernstein convex hulls conservatively reject empty
parameter intervals. Remaining fragments are subdivided nearest-first using
their conservative ray-parameter bounds. A regular root requires a non-zero
winding certificate. A tangent locator still uses determinant deflation and a
separate square-root error propagation appropriate to a double root, but a
located tangent is not returned as `Hit` when the globally audited work queue
still contains an indistinguishably near candidate.

Closed texture-cell and source-triangle domains intentionally overlap at their
boundaries. Equal root intervals are deduplicated, and the lowest stable primitive
identifier owns a compatible shared-edge hit.

## Result meanings

- `Hit`: a root certificate exists and no unresolved interval can contain a
  distinguishably closer root.
- `Miss`: every candidate fragment was conservatively rejected.
- `Exhausted`: a work limit left a potentially closer interval unresolved.
- `Invalid`: the surface, texture, ray, or geometric normal violates the contract.

The diagnostic result records the hit interval, full unresolved interval range,
an exhaustion-reason mask, fragment and subdivision counts, maximum depth,
primitive identity, and whether the singular path was used. Node-budget and
stack-capacity exits audit every pending stack entry and every remaining
fragment. A found farther root never masks a closer unresolved interval.

## Reference and tests

`CurvedExactReference.h` converts every binary64 input to an exact integer times
a power of two. For every overlapped texel cell it constructs the two projected
bivariate polynomials directly in source-triangle barycentrics, forms exact
Sylvester resultants with arbitrary-size integers, and isolates real coordinate
roots with a signed pseudo-remainder Sturm chain. This covers repeated roots,
closed texel-cell boundaries, and shared source-triangle edges. Isolated
coordinates are paired and the public surface outputs are evaluated in binary64;
positive-dimensional systems are reported as unsupported rather than guessed.

The previous two-resolution tessellation remains as a complementary geometric
and rendering reference. The deterministic fuzz prefix now compares hit
classification, nearest depth, UV, geometric normal, barycentrics, and primitive
ownership against the exact oracle; the larger fuzz corpus continues to use the
independently converged tessellation reference.

The checked-in tests cover two noncoplanar triangles, varying unit directions, a
compatible shared edge, full derivative validation, finite ray intervals,
invalid input, forced exhaustion, an analytically constructed isolated tangent,
random nearest-hit comparisons, and a grazing cost smoke test. Existing planar
tests remain separate and mandatory.

`CurvedGpu.hlsl` is a native D3D12 compute prototype over CPU-generated cell
fragments. Newton is only a locator: acceptance comes from outward-expanded
Bernstein boundary winding or Krawczyk interval inclusion. A caller supplies the
required depth accuracy, the shader combines it with scale-derived float error,
and the returned `tError` encloses the accepted leaf; the former fixed `0.01`
allowance is gone. Singular and boundary cases without an inclusion proof return
`Exhausted`. The fixed local stack uses the same globally audited pending-work
semantics as the CPU path.

## Validation snapshot

On the local AMD Radeon RX 7900 XTX validation system:

- Existing planar CPU, compute, and procedural DXR conformance remained passing.
- Curved CPU: 1,091 assertions passed. The exact resultant/Sturm oracle covers
  the repeated tangent, texel-cell boundary, shared triangle edge, and 12
  deterministic fuzz rays with full output comparisons. The complementary
  two-resolution reference converged for 298 randomized rays, including 111
  hits; the maximum was 69 subdivision nodes.
- Curved CPU grazing smoke: 1,024 constructed rays took about 1.31 seconds,
  averaging about 131 nodes/ray with p95 194 and p99 497. The same eleven named
  rays report `Exhausted` with the parameter-resolution and uncertified-leaf
  reasons. The globally audited exact-dyadic tangent also remains `Exhausted`
  after finding its singular candidate, while the exact oracle proves the root.
- Curved GPU compute: 288 mixed rays produced 209 certified hits, 48 misses,
  three invalid results, and 28 explicit exhaustions at a requested 0.5 mm depth
  accuracy. The corpus includes near misses, finite intervals, three
  multiple-intersection rays, reversed fragment order, and forced one-node
  exhaustion. A representative timestamp was about 250 ms, with a maximum of
  1,438 nodes.

These costs are intentionally unoptimized and are not evidence of a performance
advantage. They show that pre-expanded cell fragments and repeated reconstruction
of cubic controls are too expensive to adopt unchanged.

## Remaining proof gates

The numerical correctness gate is closed for the supported domain: `Hit` needs
an independent inclusion proof and a requested depth enclosure, while incomplete
work is observable as `Exhausted`. The eleven CPU grazing rays, the constructed
tangent, and GPU singular/boundary leaves without inclusion are explicit
unsupported cases, not false hits or misses.

The next gate is traversal representation and cost. Measure alternatives to the
176-byte per-cell-fragment test record, avoid reconstructing cubic controls at
every node, and choose the cooked representation only from correctness, memory,
and representative rendering-cost measurements.

## Deliberate non-commitments

The fragment representation, control nets, work queue, and diagnostic record are
research data, not the future mesh asset layout. Blender/FBX transport,
SceneAPI products, raster coverage, production Atom ray tracing, and material
integration must not depend on these structures until the GPU prototype and exact
oracle establish the necessary traversal data and error bounds.

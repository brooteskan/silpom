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
winding certificate. An isolated tangent uses determinant deflation and a
separate square-root error propagation appropriate to a double root.

Closed texture-cell and source-triangle domains intentionally overlap at their
boundaries. Equal root intervals are deduplicated, and the lowest stable primitive
identifier owns a compatible shared-edge hit.

## Result meanings

- `Hit`: a root certificate exists and no unresolved interval can contain a
  distinguishably closer root.
- `Miss`: every candidate fragment was conservatively rejected.
- `Exhausted`: a work limit left a potentially closer interval unresolved.
- `Invalid`: the surface, texture, ray, or geometric normal violates the contract.

The diagnostic result records the hit interval, nearest unresolved interval,
fragment and subdivision counts, maximum depth, primitive identity, and whether
the singular path was used. A found farther root never masks a closer unresolved
interval.

## Reference and tests

`CurvedTests.cpp` independently tessellates the evaluated surface at two
resolutions and accepts a comparison only after nearest-hit convergence. This is
independent of the candidate's cell partition, polynomial construction, bounds,
and Newton/deflation code. It is a numerical reference, not an exact algebraic
oracle; an exact dyadic resultant/Sturm oracle remains required before calling
the prototype a formal proof for unrestricted production inputs.

The checked-in tests cover two noncoplanar triangles, varying unit directions, a
compatible shared edge, full derivative validation, finite ray intervals,
invalid input, forced exhaustion, an analytically constructed isolated tangent,
random nearest-hit comparisons, and a grazing cost smoke test. Existing planar
tests remain separate and mandatory.

`CurvedGpu.hlsl` is a native D3D12 compute prototype over CPU-generated cell
fragments. It uses a fixed local subdivision stack, Bernstein bounds, Newton
refinement, determinant deflation, and the same nearest-unresolved rule. The GPU
record reports its `t` error explicitly. The current float prototype uses a
`0.01` ray-parameter enclosure for its difficult/tangent fixture; this is an
experimental error budget, not a production tolerance. The tangent ray is
required to set the singular-path diagnostic.

## Validation snapshot

On the local AMD Radeon RX 7900 XTX validation system:

- Existing planar CPU, compute, and procedural DXR conformance remained passing.
- Curved CPU: 1,021 assertions passed. The independent two-resolution reference
  converged for 298 randomized rays, including 111 hits; the maximum was 69
  subdivision nodes.
- Curved CPU grazing smoke: 1,024 constructed rays took about 1.31 seconds,
  averaging 133 nodes/ray with p95 194 and p99 497. Eleven rays correctly
  reported `Exhausted` rather than being converted to hits or misses.
- Curved GPU compute: 256 constructed hits, including an exact-dyadic tangent,
  agreed with the CPU hit enclosures. A representative timestamp was about
  52.5 ms, with a maximum of 486 nodes.

These costs are intentionally unoptimized and are not evidence of a performance
advantage. They show that pre-expanded cell fragments and repeated reconstruction
of cubic controls are too expensive to adopt unchanged.

## Remaining proof gates

The implementation proves the control flow and failure semantics, but it does
not yet justify starting the full asset/runtime pipeline:

1. Replace the convergent tessellation reference with the planned exact dyadic
   resultant/Sturm oracle, including multiplicity and boundary roots.
2. Replace sampled GPU Newton/deflation acceptance with outward-rounded interval
   arithmetic or another independently justified inclusion test.
3. Reduce the GPU tangent enclosure from `0.01` to a scale-derived bound suitable
   for hit depth and primitive ownership.
4. Resolve or conservatively accelerate the 11 difficult CPU rays that exhaust
   despite the larger work budget.
5. Measure alternatives to the 176-byte per-cell-fragment test record before
   defining any cooked representation.

## Deliberate non-commitments

The fragment representation, control nets, work queue, tolerances, and diagnostic
record are research data, not the future mesh asset layout. Blender/FBX transport,
SceneAPI products, raster coverage, production Atom ray tracing, and material
integration must not depend on these structures until the GPU prototype and exact
oracle establish the necessary traversal data and error bounds.

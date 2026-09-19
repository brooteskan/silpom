# Curved traversal and authoring implementation status

This work extends `efbaf7c` on the local main checkout. It does **not** enable a
curved runtime component or close issue #1. The planar runtime is unchanged.

## Implemented traversal experiments

`Tests/Curved/CurvedPacking.h` provides three test representations:

| Representation | Source triangle | Fragment | Two-triangle, 22-fragment fixture |
| --- | ---: | ---: | ---: |
| Legacy repeated geometry | embedded | 176 bytes | 3,872 bytes |
| Shared geometry + cell polynomial | 128 bytes | 64 bytes | 1,664 bytes |
| Shared geometry + polynomial + cached bounds | 128 bytes | 80 bytes | 2,016 bytes |

These are experimental buffer layouts, not serialized assets. Bounds are resolved
for the exact height data, base scale, amplitude, reference, and address mode used
to pack the mesh. Changing those inputs requires repacking; applying an ordinary
entity scale to cached bounds is not equivalent to metre-based displacement.

The compact path evaluates the cell's bilinear polynomial directly. Quadratic
height controls multiplied by the linear direction field give cubic Bernstein
controls algebraically, without reconstructing them from ten texture samples at
every node. Newton uses analytic surface derivatives, including `h * derivative(D)`.
Tests compare the control nets to independent sampled reconstruction over signed
amplitudes, positive scales, and clamp/repeat modes.

Optional cached bounds reject fragments before polynomial traversal. Direct
interval evaluation now certifies regular roots on a box covering a whole child,
requiring the root enclosure to remain inside the source triangle and texel cell.
A separate bounded exact-arithmetic dispatch handles supported quadratic and
singular leaves, with a full nearest-hit audit. The earlier control-net inclusion
and winding paths remain available. The main 48-entry stack retains explicit
capacity diagnostics. See [CurvedCoverageGate.md](CurvedCoverageGate.md).

The returned UV and normal are evaluated on the GPU and verified against the CPU
reference. They are no longer reconstructed on the CPU as a substitute for testing
shader output. The harness checks the requested depth budget (`5e-4` in ray
parameter units, plus its scale-derived numerical floor), replacing the old
ten-times-looser assertion. Non-unit ray directions must be accounted for when
translating a ray-parameter error into metres.

## Results and unresolved tradeoff

Measured on an AMD Radeon RX 7900 XTX using the Release standalone D3D12 harness.
Each warm benchmark reports the median and maximum of five GPU timestamps; these
are tiny research workloads, **not** frame times or evidence of production speed.

Representative runs with the default subdivision floor:

| Workload | Compact, bounded | Legacy representation with current common solver |
| --- | ---: | ---: |
| 192 front rays | about 1.9 ms | about 24 ms |
| 25 grazing rays | about 16 ms | about 91 ms |

The original 288-ray mixed corpus now returns 237 certified hits, 48 misses,
3 invalid results, and zero exhaustions. All 28 previous failures are resolved.
Candidate reversal and forced node-budget exhaustion remain tested. The
three-budget sweep exercises 210
cases where a found candidate must remain `Exhausted`; it also caught and fixed
an exit that incorrectly reported exhaustion after every remaining candidate had
been rejected. Budget limits alone no longer change a proven miss into a failure.
CPU planar and procedural planar DXR
conformance remain mandatory and passing; there is still no curved DXR adapter.

Analytic controls permit a finer parameter floor, retained as an explicit test
mode. Direct interval evaluation now completes all 48 rays in each nonzero
bilinear-cross-term group at base scales 0.125 and 8, for both amplitudes `-0.04`
and `+0.04`. The fine-mode grazing benchmark is about 26 ms. General singular
cubics and non-affine seam ownership are still outside the demonstrated coverage;
these tiny workloads do not establish production traversal viability.

The follow-up CPU coverage work resolves the exact-dyadic tangent and all eleven
named grazing failures using exact rational and interval-inclusion certificates.
All 1,024 CPU grazing rays now complete. See
[CurvedCoverageGate.md](CurvedCoverageGate.md) for proof domains, boundary sweeps,
separate error budgets, and remaining limitations. The oracle's exact elimination
and root isolation still use numerical coordinate pairing and attribute evaluation.

## Reproduce

From the Gem directory, use the same installed DXC as the existing GPU tests:

```powershell
cmake -S . -B build/curved -DSILPOM_DXC=<absolute-dxc-path> -DSILPOM_PYTHON=<absolute-python-path>
cmake --build build/curved --config Release
ctest --test-dir build/curved -C Release --output-on-failure
```

GPU output includes both representations, fast/fine grazing timings, actual
shader attribute checks, signed displacement, and base scales 0.125 and 8.
CPU tests additionally validate packed sizes, reversible candidate order,
conservative cached bounds, invalid heights, and explicit preprocessing budgets.
The current CPU target passes 16,607 assertions, including the existing exact and
convergent-reference cases. The metadata target passes six decoder unit tests.

## Remaining implementation gates

1. Resolve required singular/boundary/grazing rays without losing certification;
   expand exact-reference and seam/silhouette sweeps. A diagnostic is not a visible
   surface, so these cases cannot simply become transparent runtime pixels.
2. Evaluate hierarchical/lazy cell traversal and cached control-net subdivision
   over realistic mesh counts, UV tiling, and texture resolutions. The measured
   small fixtures are still far too costly to choose a production representation.
3. General authoring is now implemented and tested separately; see
   [ExportImportRoundTrip.md](ExportImportRoundTrip.md). Production SceneAPI
   products, sidecar source dependencies, metadata stripping, filtered ordinary
   geometry, and coherent asset publication are not implemented.
4. After those gates, implement batched conservative raster coverage, materials,
   depth/color/shadow agreement, curved procedural DXR, independent rendered
   reference comparisons, and the complete issue acceptance scene.

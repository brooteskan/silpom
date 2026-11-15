# Spatial grouping inside a paint entity

Paint documents, layers, stroke order and the rolling 100-dab edit history are
unchanged. Consolidated dabs remain resolved projections, rather than baked
textures. Both consolidated and live projections enter the same renderer.

The renderer builds conservative world-space boxes from the eight corners of
each oriented projection, including entity rotation and uniform scale. It then
partitions their union with an adaptive spatial BSP. Surface-area cost chooses
useful splits without using material identity or chronological proximity. Dense,
coincident paint remains one group. Leaves with at most 128 references stop
splitting; at most 32 native composite handles and twice the original projection
references are allowed per entity.

Each leaf filters the original ordered projection stream. A projection crossing
a split occurs in both children. Shader clipping assigns the split plane only to
its upper child, so a surface point evaluates exactly one group. This preserves
noncommutative blends, normal blending, per-layer erase baselines, height masks,
and the reverse alpha fast path regardless of native tile-list ordering. Native
culling boxes are trimmed to the leaf's occupied bounds and conservatively padded.

All groups share one raw GPU buffer. Each native composite's texture index holds
its header byte offset; projection offsets are absolute, node links are local.
Each clipped header has flag 8 and a 32-byte clipping record after its projections:
low.xyz, inclusive-upper-axis mask, high.xyz, reserved. Material anchors remain
one per distinct material asset, retaining Atom's shared texture arrays. Grouping
does not allocate or copy decal textures.

Stamps affect color and normals only. They do not modify the SiLPOM heightmap,
relief geometry, learned data or relief acceleration bounds. Height masks only
select which existing relief heights can receive paint.

## Diagnostics and reproducible comparison

- `r_silpomPaintSpatialGrouping 0` retains the single-volume ordered-BVH baseline.
- `r_silpomPaintSpatialGrouping 1` enables automatic grouping (default).
- `r_silpomPaintCompositeReference 1` forces the existing brute-force forward
  reference, overriding grouping and the alpha fast path.
- `DecalPaintRequestBus.GetRenderStatsJson` reports batches, distinct materials,
  unique projections, projection references, buffer count/bytes and rebuild time.

`Tests/DecalPaintTests.cpp` checks exact candidate order against a brute-force scan
at thousands of projection corners, cell faces, shared split planes, adjacent
floating-point values and a projection spanning every cell. It also checks empty
paint, the ungrouped baseline, dense overlap and the reference/handle caps.

`Tests/decal_spatial_benchmark.py` runs in a dedicated Editor and never saves
VueLiveLink. Set `TG_SILPOM_ESTIMATOR_OUTPUT` to an evidence directory and run it
with Editor's `--runpython` argument. Use `TG_PAINT_GROUPING_MODES=original,original`
for the original binary, or `0,1,1,0` for a matched ABBA comparison in one process.
It retains tower quality settings, uses two fixed gameplay cameras at 1280×720
with 2× MSAA, and captures 180 GPU timestamp frames per run after warm-up. An
invisible benchmark reference mesh enables timestamp collection without adding a
visible surface. Screenshots and raw diffuse/normal attachments are captured
outside timed intervals. The fixtures cover the saved paint and 4,096 dabs spread
around the tower, reusing its material, with two layers, mixed blends, normal
paint, erase operations and height masks.

`Tests/analyze_decal_spatial_benchmark.py BEFORE AFTER` summarizes per-run medians
and checks raw diffuse/normal equality between grouped and ungrouped rendering.
Graphics timestamp span includes waits between graphics passes; the reported CPU
frame period includes waits and is not paired to the delayed GPU query frame.
These are neither isolated CPU work measurements nor guaranteed frame rates.

The updated shader is supplied in `Integration/O3DE/engine.patch`; its manifest
hashes and install tests cover LF and CRLF checkouts. Rebuild shader dependencies
after applying the integration, including generated forward shaders. A local
engine snapshot excluded from broad project scans can require explicit shader
reprocessing through AssetProcessorBatch's `--reprocessFileList` option.


## Measured results, 2026-11-15

RX 7900 XTX, Ryzen 7 2700, profile build, DX12 SM 6.7, gameplay at
1280×720 with 2× MSAA. The table uses the matched ABBA run's GPU graphics-span
medians (two runs per mode, 180 frames per run):

| Paint / camera | Single volume | Grouped | GPU time reduction |
| --- | ---: | ---: | ---: |
| Saved 12,940 dabs / inside | 7.26 ms | 6.98 ms | 3.9% |
| Saved 12,940 dabs / outside | 6.17 ms | 6.02 ms | 2.4% |
| Spread 4,096 dabs / inside | 33.62 ms | 19.69 ms | 41.4% |
| Spread 4,096 dabs / outside | 7.94 ms | 6.60 ms | 16.9% |

The original binary, measured before implementation, was 7.48 / 6.14 ms for
saved inside/outside and 35.94 / 8.04 ms for spread inside/outside. The matched
single-volume toggle controls for session drift and the new shader's header
handling. The spread case stresses broad exterior bounds and interior rejection:
most interior pixels have no stamp contribution, but previously still paid for
the broad composite traversal. Saved paint has substantial visible interior paint.

Saved paint selected 32 groups, 17,042 references for 12,940 unique dabs, and
2,727,232 GPU bytes versus 2,070,384 bytes. Spread paint selected four groups,
4,600 references for 4,096 dabs, and 736,064 bytes versus 655,344 bytes. Both
retained one material anchor and one shared GPU buffer; no texture duplication.

Individual Editor rebuild observations were about 8–9 ms ungrouped versus
41 ms grouped for saved paint, and 3 ms versus 7.5 ms for spread paint. These
include buffer creation/upload waiting and are not statistical CPU-work samples.
They expose the edit-time tradeoff; steady-state rendering does not rebuild.

In all four camera/paint comparisons, every tower hit record was bit-identical,
and zero valid tower pixels changed in diffuse or normal outputs. Entire inside
attachments were identical too. Outside attachments had small differences on
non-tower scene pixels between captures. The input prefab hash remained unchanged.

[Machine-readable results](Benchmarks/SpatialDecalGrouping-20261115.json) retain
all per-run medians, resource counts, shader hashes/profiles and validation scope.
Raw timestamps and images are under `scratch/decal-spatial` in the TG workspace.

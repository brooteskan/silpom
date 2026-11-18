# Conservative batch resolve regions

`r_silpomResolveRegions` selects the screen-region scheduler for
`SP_TOWER_LOD_4PX_RESOLVE` (`surface.maxCells = 65525`). It defaults to enabled;
disable it to compare against the original full-screen batch draws. Other
resolve modes retain their existing draw items.
`r_silpomResolveMultiRect` defaults to enabled and selects disjoint rectangles
from face clusters. Disable it to retain the first implementation's single
rectangle per batch while leaving `r_silpomResolveRegions` enabled.

`SilPOMResolvePass` uses the ordinary `silpomHitForward` draw list. A scene
registry associates each published batch with its owner range, immutable
coverage bounds, object transform and current mesh draw packet. The registry
does not group material assets: separate objects and owner ranges remain
separate batches. Packet update notifications follow shader variants, pipeline
state changes and receiver UV paint material swaps.

The pass replaces each registered item with its own disjoint rectangle draws.
Each replacement clones the original packet, copies its geometry view and
submits the existing first three indices. It preserves the source item's
sort/depth/filter properties, instance arguments, stencil reference, shader
variants, SRGs and root constants. The original item remains available for
Atom's ordinary view routing but is not also submitted by this pass. Unregistered
items and unsupported packet shapes use the original path.

Each batch retains a conservative bound for every face, as well as its complete
union. These bounds enclose all displaced corner extrema, authored proxy planes, distant
shells, recessed wall backup and diagnostic records. They belong to the
published generation, including its baked displacement scale. A replacement
configuration cannot alter the old generation's resolve bounds while loading.
Rigid transform updates move the registry bounds with the rendered object.

Projection uses the current view's jittered world-to-clip matrix, after the
view SRG update. Each face bound is clipped conservatively to a relaxed near
half-space before perspective division; every pair of corner vertices is
considered so no clipped-hull edge intersection is missed. A uniform homogeneous
error interval covers float transform uncertainty. Whole bounds provably
outside near/far depth are rejected; uncertain positive W retains the complete
drawable viewport. Remaining far clipping is omitted conservatively.
Rectangles round outward, include coverage numerical padding,
two pixels for non-cascading hole repair, and a separate derivative guard. They
align outward on the absolute render-target pixel-quad grid before clipping to
the existing viewport, scissor and attachment dimensions. Their union is marked
on an absolute 32-pixel grid and merged into non-overlapping row-run rectangles.
The grid is conservatively coarsened when necessary to stay within 32 rectangles
per batch. Internal boundaries always fall between complete derivative quads.
No expansion occurs after the partition, so an output sample is never shaded
twice by one batch. Empty support uses an empty scissor.

The expanded list is prepared in `SetupFrameGraphDependencies`, after current
view SRGs are updated and before Atom partitions command recording. The base
raster pass therefore receives the exact expanded draw count. All original
draw properties remain attached to each replacement rectangle.

The viewport, full-screen primitive, sample-qualified NDC, material shader
control flow, hit lookup, recorded depth output, depth/stencil state and repair
passes are unchanged. Scheduling covers complete pixels for every MSAA mode;
the shader still accepts owners independently for each sample. There is no
early depth test against the primitive's raster depth of 0.5.

Per-pass packet/geometry/scissor storage is never written into a shared mesh
packet. Resources are retained through submission and regenerated each frame,
so resizing, projection changes, sample-count transitions and pipeline rebuilds
cannot reuse stale rectangles. The source RHI packet, geometry buffers,
material, shaders and bound SRGs/PSOs are retained explicitly. No mesh packet
event handlers are copied.

Whole quads and the guard preserve the existing derivative neighborhood. The
existing ownership discard precedes some gradient calculations; this change
does not define gradients across ownership discontinuities or promise bitwise
hardware equivalence. Runtime A/B comparisons must check shaded results as
well as ownership/depth, especially at material and proxy boundaries.

Validation includes the standalone `SilPOM.ResolveRegion` and
`SilPOM.ResolveRegions` CTest targets. The latter exercises near clipping,
uncertainty, randomized union coverage, disjointness, quad alignment and the
rectangle budget. The
runtime harness `Tests/TowerResolveRegionsEditorSmoke.py` and validator
`Tests/test_tower_resolve_regions.py` compare the full-screen reference with
region scheduling using per-sample attachments. GPU measurements are recorded
below.

## Validation, 2026-11-16

- Standalone `SilPOM.ResolveRegion` CTest passed, including 40,000 randomized
  projected points with clip uncertainty, repair destinations and derivative
  partners, plus explicit eye/clip-plane, offset and odd-size cases.
- The integrated C++ unity translation unit compiled with MSVC C++20, `/W4`,
  `/WX`, `/fp:precise` and the project's definitions/includes. The static
  library and SilPOM, Editor and Builder DLLs linked. The local PDB manager
  mismatch was avoided with embedded `/Z7` debug information and no linker PDB;
  no source or project build flags were changed for that workaround.
- AssetProcessorBatch processed 120 assets with zero failures/errors, including
  the custom forward pass template.
- Ten DX12 A/B cases passed: 1x and 2x, each at exterior, lower-facing,
  lower-glancing, upper-glancing and 1281x721 exterior views. All five
  per-sample attachments matched exactly: hit records, depth, diffuse, specular
  and normals. Repeated full-screen controls were stable. The cases contained
  8,417,013 owned samples in total; the saved level remained unchanged and
  viewport/render settings were restored.
- Two experimental raw-coverage 4x cases passed at exterior and lower-glancing
  views, covering another 3,783,626 owned samples. All five attachments and
  repeated controls matched exactly, settings were restored and the saved
  level was unchanged. Across all 12 cases, 12,200,639 owned samples matched.

The retained results are under
`TGProject/user/silpom-resolve-regions-20261116/{results.json,report.json,Editor.log}`.
The additional 4x results use the sibling directory with suffix `-4x`.
For example, the exterior trace replaced all 14 registered items with
three-index draws and scheduled 301,560 rectangle pixels versus 12,902,400
full-screen batch pixels. These are coverage counts, not measured GPU time.

Reproduce with a dedicated Editor using
`--runpython <gem>/Tests/TowerResolveRegionsEditorSmoke.py`,
`SILPOM_RESOLVE_OUTPUT` set to a fresh output directory, and then run
`python Tests/test_tower_resolve_regions.py <output> <output>/report.json` from
the gem directory. Set `SILPOM_RESOLVE_SAMPLES=4` to check the experimental
raw-coverage 4x route separately. Use the same canonical project path as the
asset database when launching the asset processor and Editor.

## Single-rectangle GPU benchmark at 2x MSAA, 2026-11-16

The same compiled Editor binary was measured with
`r_silpomResolveRegions=0` (original resolve geometry and coverage) and `1`
(three indices plus conservative scissors). Hardware: Ryzen 7 5700G integrated
AMD Radeon Graphics, device 0x1638, driver 31.0.21923.11000, 32 GiB RAM
configured at 2133 MHz. The renderer used DX12/SM 6.2 at 1280x720, 2x MSAA,
SMAA/TAA off. Height-derived normals were enabled in both modes; all other
tower materials, lighting, Level 4B/5A/5B and quality controls were unchanged.
The effective configuration hash was
`b76f275b9af10b14731ac7dc5115e1e29eb6edb11cebd57cd05e22b3b32ec67e`.

For each fixed fixture camera, three pairs of 120-frame captures were
interleaved in alternating order within one Editor session. This gives 360
accepted frames per mode/view, 2,880 in total. Each camera settled for 90
frames, each mode for 30 frames, and the native timestamp collector discarded
another 16 frames. Native GPU pass timestamps were used; inspection atlases,
screenshots, RenderDoc and asset compilation were excluded from measurement.
Every capture retained 120 fresh timestamps for each leaf pass, with no
duplicate or missing samples. Renderer source, shader, binary and saved scene
hashes stayed unchanged.

SILPOM forward resolve, in milliseconds. Each time is **median / p95**.
Speedup is original median divided by region median.

| View | Original resolve | Region resolve | Speedup | Median time reduction |
| --- | ---: | ---: | ---: | ---: |
| Exterior | 23.132 / 25.542 | 4.806 / 5.019 | 4.813x | 79.22% |
| Close interior, facing | 102.905 / 106.816 | 96.670 / 98.825 | 1.064x | 6.06% |
| Close interior, lower glancing | 109.237 / 113.468 | 98.376 / 100.456 | 1.110x | 9.94% |
| Close interior, upper glancing | 94.426 / 96.268 | 88.108 / 89.526 | 1.072x | 6.69% |

All three capture pairs favored the region implementation in every view.
Paired resolve time-reduction ranges were 79.06-79.47%, 5.56-6.32%,
9.79-10.14% and 6.51-6.75%, respectively. These are repeat ranges, not
independent-frame confidence intervals; frames within a capture are correlated.

The first-to-last graphics-pass timestamp span also improved:

| View | Original median, ms | Region median, ms | Speedup | Time reduction |
| --- | ---: | ---: | ---: | ---: |
| Exterior | 64.896 | 45.529 | 1.425x | 29.84% |
| Close interior, facing | 183.730 | 177.450 | 1.035x | 3.42% |
| Close interior, lower glancing | 253.087 | 241.588 | 1.048x | 4.54% |
| Close interior, upper glancing | 230.465 | 224.471 | 1.027x | 2.60% |

Graphics span includes queue gaps. It is not GPU busy time, a sum of nested
passes or an FPS measurement. Hit-generation median changes were below 0.4%
in every view. The full report also retains p95 values, every capture median,
paired comparisons, other leaf timings and delayed RHI frame periods.

The exterior trace submitted 301,560 rectangle pixels across 14 batches
instead of 12,902,400 full-screen batch pixels. The close facing view retained
the full viewport for all eight batches; the two glancing views retained about
89% of full-screen rectangle area. Thus this first rectangle-per-batch stage
benefits the exterior most. Timings measure the combined scissor and
three-index implementation; they do not independently attribute the saving
to pixel coverage versus vertex/primitive submission.

A separate normals-on validation run passed all four views: 5,541,526 owned
samples retained identical hit records, depth, diffuse, specular and normals.
Repeated original-resolve controls were stable. Normal, AA and viewport
settings were restored, and the saved level remained unchanged. Validation
readbacks ran after the timing session and could not affect these measurements.

Raw captures, machine inventory, hashes and `report.json` are retained in
`TGProject/user/silpom-resolve-benchmark-2x-20261116`; the `quality` subdirectory
contains the separate validation. A [compact measurement and provenance record](../Tests/Reference/Ryzen5700GResolveRegions20261116.json)
is retained with the tests. Reproduce with
`Tests/TowerResolveRegionsBenchmark.py` and
`SILPOM_RESOLVE_BENCH_OUTPUT` pointing to a fresh directory, then run
`Tests/analyze_resolve_regions_benchmark.py <output> --output <output>/report.json`.
The separate `Tests/TowerResolveRegionsNormalsValidation.py` harness checks
sample equality with the same effective normals-on configuration; its output
uses the existing `Tests/test_tower_resolve_regions.py` validator.

## Disjoint-rectangle GPU benchmark at 2x MSAA, 2026-11-16

The disjoint implementation passed strict MSVC C++20 `/W4 /WX /fp:precise`
compilation, runtime/Editor linking and both standalone resolve CTest targets.
The same four cameras, 1280x720 viewport, 2x MSAA, normals-on configuration,
hardware and interleaved three-pair/120-frame protocol above were rerun.
`r_silpomResolveRegions=0` selected the original resolve; `1` selected disjoint
rectangles with `r_silpomResolveMultiRect=1`. All 2,880 timed frames passed
freshness, metadata and hash checks. Every pair favored disjoint scheduling.

Forward resolve time in milliseconds, **median / p95**:

| View | Original resolve | Disjoint resolve | Speedup | Median time reduction |
| --- | ---: | ---: | ---: | ---: |
| Exterior | 23.365 / 26.471 | 4.688 / 4.740 | 4.984x | 79.94% |
| Close interior, facing | 105.647 / 109.486 | 92.502 / 93.110 | 1.142x | 12.44% |
| Close interior, lower glancing | 111.641 / 114.845 | 92.808 / 93.900 | 1.203x | 16.87% |
| Close interior, upper glancing | 97.340 / 99.076 | 82.284 / 82.794 | 1.183x | 15.47% |

Graphics-pass timestamp span (including queue gaps, not GPU busy time or FPS):

| View | Original median, ms | Disjoint median, ms | Speedup | Time reduction |
| --- | ---: | ---: | ---: | ---: |
| Exterior | 61.873 | 43.059 | 1.437x | 30.41% |
| Close interior, facing | 186.415 | 173.101 | 1.077x | 7.14% |
| Close interior, lower glancing | 255.437 | 236.725 | 1.079x | 7.33% |
| Close interior, upper glancing | 233.261 | 218.093 | 1.070x | 6.50% |

The three interior paired resolve reductions ranged 12.37-12.58%,
16.79-17.04% and 15.28-15.58%, respectively. Hit-generation median changes
were below 0.1% within the current original/disjoint comparison.

The separate exact-sample validation passed all four views: **5,541,526 owned
samples**, zero differences in hit records, depth, diffuse, specular or normals,
and stable repeated-original controls. Normals, renderer settings and viewport
were restored. Saved scene, renderer source, shader and binary hashes stayed
unchanged throughout validation and measurement.

The exterior used 32 rectangles for 14 batches (277,504 scheduled pixels).
Facing used 22 for eight batches (2,929,152 pixels, one empty draw); lower
glancing used 21 for nine batches (2,371,584); upper glancing used 17 for nine
batches (2,206,208, two empty draws). The corresponding full-screen batch
areas were 12,902,400 / 7,372,800 / 8,294,400 / 8,294,400 pixels. These counts
describe coverage, not predicted GPU time.

Compared with the **earlier, separate** single-rectangle run, close-view
resolve medians fell 96.670 to 92.502 ms, 98.376 to 92.808 ms and 88.108 to
82.284 ms (4.31% / 5.66% / 6.61%). Original-control medians also shifted
2.2-3.1% between sessions, so this historical comparison does not isolate
the entire difference to the scheduler. The current paired comparison above
is the controlled speedup measurement.

Artifacts are in `TGProject/user/silpom-multi-rect-2x-20261116`, including
`report.json`, raw timestamps, `historical-single-comparison.json`, integrity
checks, logs and the separate `quality/report.json`. The
[compact measurement record](../Tests/Reference/Ryzen5700GMultiRectResolve20261116.json)
retains the timing and quality results with provenance.

The later [winning-sample gradient correction](GradientContext.md) changes
material filtering and adds sidecar/producer work. The timings and exact legacy
shading comparison above describe the frozen pre-correction build. They are not
measurements of the corrected gradient path.

The refined normal-only build has a separate [2× MSAA timing report](NormalOnlyResolveRyzen5700G.md), with the same four fixed views and 3×120 frames per mode. Both reference and rectangle paths use the corrected shading contract.

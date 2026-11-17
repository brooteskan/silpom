# Shared disjoint repair rectangles

`SilPOMRepairClassifyPass` and `SilPOMRepairFillPass` retain the existing
fullscreen triangle, shaders and full-sized attachments. They share
one immutable frame-local execution decision: full-screen fallback, disjoint
regional scissors, no work, or an unavailable pair requiring reconstruction.
`r_silpomRepairRegions=false` selects full-screen repair.
`r_silpomRepairRegionTrace=true` reports the plan without GPU measurements.

The depth raster pass snapshots its actual combined, enabled, draw-filtered
list after final view state is available. Registration covers every hit-reuse
mode; forward batch replacement remains limited to mode 65525. Published
geometry/profile bounds include displaced extrema and numerical allowances.
Known wall-producing implicit, base/grazing and ordinary shell variants are
included, including the default static-wall shader override. Pure proxies are
explicitly accounted for, while post-fill wall backup is excluded. Missing
registrations, unknown shaders, viewport overrides and unsupported draw state
force full-screen repair. An empty registry alone never proves no work.

Projection uses each generation's complete published per-face coverage bounds,
the actual jittered view, retained object transform and CPU/GPU roundoff bounds.
Missing or empty coverage clusters use the complete batch AABB. Donor coverage
is clipped to each producer's own actual viewport/scissor and target, expanded
by two pixels in both axes, then clipped to the repair domain. Donors outside
the repair scissor can still support an inside repair.

The existing conservative grid union partitions this support into disjoint
half-open rectangles, starting on a 32-pixel grid and coarsening outward to
keep at most 32 draws per pass. Accumulation also coarsens every 1,024 supports
to bound temporary storage. Coarsening can add pixels, never remove support.
This retains separated islands instead of enclosing every donor in one box.
Repair itself adds no derivative-quad requirement.

Baseline execution and regional coverage have separate contracts. The baseline
requires usable shaders/draws, compatible samples, all three scratch outputs,
hit/gradient/depth identity, complete classifier sample coverage and the original
fill depth test. Each scope snapshots its declared attachment identities,
formats, subresources, load/store actions, access and dependency stages during
setup. Compilation checks those declarations against the compiled graph and
the prepared PSO/SRG image bindings. An incompatible baseline cancels both draws
and queues both passes for reconstruction. A missing or disabled partner also
queues reconstruction without waiting for a callback that cannot arrive.

Setup reserves `max(1, candidate rectangle count)` submissions for both scopes,
including candidate no-work, and retains every attachment declaration and
classify-to-fill dependency. Execution groups are constructed before resource
compilation, so compilation never changes this reservation.

Each pass performs normal resource preparation, then the second serial compile
completion seals and publishes the same decision to both passes. Regional and
no-work decisions require the complete current inventory, its retained generation
sources, the same view and exact projected view matrix, and unchanged domains.
Uncertain evidence selects a valid full-screen pair. Full-screen classification
uses the intended fill viewport and scissor, including fractional viewport edges
and matching sample state; it never intersects away part of the fill domain.
The classifier uses screen coordinates and global donor reads, so this domain
alignment preserves its calculations.

Recording only consumes the sealed decision and retained draw, geometry, PSO,
SRGs and compiled image views. It performs no live partner validation. Regional
draws respect their indexed submission ranges; fallback submits exactly index
zero from the range containing zero; no-work submits nothing. Unused reserved
slots remain unused. Shader reload notifications during rendering are deferred
until initialization. All classification draws complete before any fill draw.
Each pass keeps one attachment scope, so additional rectangles do not repeat
clears or discard earlier scratch writes. Producer/resource references are
retained with the decision and eight retired frames.

One-times-MSAA scratch clears remain on all three targets even with no draws.
MSAA DontCare remains unchanged: every sample fill may read is initialized by
classification, across either the shared rectangles or the entire fallback
domain. Every MSAA rejection/no-repair path still writes zeros to all three
scratch outputs, including the greater-than-two-sample ownership gate. Outside
the selected domain scratch may be undefined.
Donor reads remain global, sample-frequency execution and all-center-sample
rejection stay unchanged, and fill retains GreaterEqual depth testing and Load
on its destinations. All existing hit/gradient/depth publication is unchanged.

Focused CPU tests cover diagonal reach, boundary donors, separated islands,
overlapping inputs with disjoint output, draw-budget coarsening, bounded input
accumulation, uncertainty/fallback and empty support. They include 54,423
visible donor neighborhoods across 2,000 randomized near/far projections and
227,122 union pixels across 200 randomized multiple-island cases. Native
integration compiles with the project's strict MSVC settings. The shared
execution regressions add 368 checks for preparation order, immutable sealing,
baseline unavailability versus coverage fallback, reserved capacity, partitioned
regional/full-screen submissions, and candidate no-work becoming full-screen.
All three focused repair CTest suites pass. This extension changes no repair
shaders or pass assets.

Before the fallback/sealing changes, the focused native 2x MSAA check at
1280 x 720 passed for exterior,
lower-facing, lower-glancing and upper-glancing tower views with AngularEdges4,
height-derived normals, face caches and disjoint forward resolve held fixed.
Full-screen repair, disjoint repair and the repeated full-screen control match
bit for bit: all hit records and depths, plus diffuse/specular/normal outputs
over 5,541,526 owned samples. Traces confirm active regional plans: four
rectangles outside and one merged rectangle in each close interior view.
The saved level is unchanged and temporary settings were restored. See
`Tests/Reference/RepairRegions20261117.json`; raw captures and the comparison
report are in `TGProject/user/silpom-repair-correctness-20261117`.
Those GPU captures have not been repeated for the fallback/sealing changes.
No performance timestamps were collected. Benchmarking remains deferred.

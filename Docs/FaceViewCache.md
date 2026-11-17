# Face and view preparation cache

The cache separates geometry work that belongs to a mesh generation from
coverage work that belongs to one render frame and view. The current
implementation targets the tower's winning-hit route with Maximum cells
`65525` (`SP_TOWER_LOD_4PX_RESOLVE`). It prepares implicit near-depth proxy
corners and the conservative raster-depth bound before hit-depth rendering.

The eligible forward resolve already uses its existing full-screen triangle
and disjoint batch rectangles. It does not need cached face coverage. It can
use the companion coefficients during selected-face reconstruction and
gradient evaluation. Shell and recessed-backup gradient construction in the
shared depth material can also use those coefficients. Authored shell and
learned/authored proxy coverage remain outside the first per-view cache: their
clip W, UVs and sample-interpolated plane depth retain their established paths.
The separate learned-proxy material root retains its existing coefficient
arithmetic as well.

## Records and lifetimes

The canonical 144-byte packed face descriptor and its local-face ordering are
unchanged. `MeshController` allocates a companion coefficient buffer from the
same final packed batch and retains it with that generation's resources. Its
initial metadata is unavailable. A GPU work item loads the packed face and
initializes a record only when its ready bit or version is absent.

| Record | Size | Contents | Lifetime |
| --- | ---: | --- | --- |
| Face coefficients | 48 bytes/face | `D_u.xyz`, signed UV area; `D_v.xyz`, base Newton tolerance; ready/validity/version metadata | Published mesh generation |
| Implicit face/view result | 48 bytes/face | Four final FP32 NDC corners, FP32 reverse-Z raster bound, live/empty/unavailable status and padding | One prepared frame/view allocation |
| Batch lookup entry | 32 bytes/batch | Owner base/count, result offset, face-buffer binding, coefficient-buffer binding, generation identity and availability | Same frame/view allocation |

Coefficient validity keeps the distinct existing Newton and gradient rules.
An unusable record falls back to the original calculation; it is not a new
Newton rejection or visibility test. The requested tolerance multiplier stays
at the Newton call site. Height values, height-cell slopes, ray state, iterates
and per-sample gradient contexts remain dynamic. See
[FaceCoefficientPreparation.md](FaceCoefficientPreparation.md) for the record
flags, frozen expressions and focused arithmetic proof.

For `G` generation-owned faces, `F` faces in participating batches for one view
and `B` such batches, the added raw record storage is:

```
generation coefficients: 48 * G bytes
one frame/view:          48 * F + 32 * B bytes
combined current data:   48 * G + 48 * F + 32 * B bytes
```

`F` counts all faces in eligible depth-draw-list batches, including entries
that the GPU later marks empty. This excludes allocator alignment, SRGs,
resource views, dispatch records and the existing face/selection buffers.
Current allocations and eight retired frame allocations can coexist; multiple
participating views/pipelines add their own frame-local storage. The coefficient
buffer is shared by those preparations. There is no cross-frame reuse of the
coverage result.

## Publication and render ordering

`FaceCacheBatchInputs` extends the published batch registration with retained
face/coefficient/proxy-selection buffers, actual bindless indices, face count,
and the profile values needed by coverage. The batch identity is the published
scene/object generation and owner range, not a material asset or the latest
pending editor configuration. Rigid transform updates and replacement proxy
selection snapshots update that registration. Pending generations remain
unpublished until their complete draw packets are ready; replacement publication
retires the prior generation together with its consumers.

`SilPOMFaceCachePass` is a graphics-queue compute pass inserted immediately
before `SilPOMHitDepth`. It selects enabled, published depth batches from the
actual render view's draw list and snapshots their inputs. It runs during frame
graph preparation, after the view SRGs have received the final shader matrices.
Each dispatch binds that actual view's `ViewSrg` directly, including its current
projection and jitter, and uses the matching published object transform. It
does not reconstruct a view from a camera name or reuse another eye's matrix.
The first implementation accepts a view tag only when it resolves to exactly
one actual view; ambiguous multi-view tags retain the reference path.

The sequence is:

1. Select the published batch, packet resources, transform, profile and
   proxy-selection snapshot for the current pipeline/view/tick.
2. Allocate a fresh result buffer and batch table. The small table uses a
   host-visible dynamic buffer and `UpdateData`; it does not wait for a GPU
   readback or an asynchronous initial-data upload. The generation's initial
   face/coefficient upload remains part of generation preparation.
3. Dispatch one work item per face, in one dispatch per participating batch.
   A work item conditionally prepares its immutable coefficients, then computes
   the four coverage corners and independent depth certificate.
4. Declare compute writes and later vertex/pixel reads in the frame graph.
5. Bind the matching current allocation to the depth/forward pass SRGs and
   retain its resources through consumption.

Every preparation declares coefficient access as compute read/write. This
serializes first-use initialization across views on the GPU; no CPU
"coefficients finished" flag or first-view assumption authorizes a read.
The result buffer is a compute write followed by raster shader reads. Original
face buffers and optional proxy-selection buffers also declare their compute
and raster read stages after import, so RHI transitions include their bindless
pixel/vertex reads. Availability is published to raster SRGs only after every
dispatch has an executable PSO and completely configured SRG. That flag means
submission is ready, not that the GPU has finished: the frame-graph dependencies
provide the write-to-read ordering. This relies on the pinned engine's serial
prep-before-depth `CompileProducers` ordering.

The enclosing allocation retains the actual view, shader, dispatch SRGs,
buffer views, pipeline states and selected packet/resource snapshots. The
preparation pass keeps eight retired allocations in addition to the current
one. Raster consumers retain their selected allocation until their next frame
setup, and RHI deferred destruction still applies. Shared material bindings
never receive a view-local result buffer.

## Lookup and fallback

Native lookup requires the same pipeline, actual view object and current
render tick. The shader's batch table lookup matches the owner base, canonical
face-buffer binding and local-face range. Generation identity belongs to the
enclosing published allocation and is also carried in the table. The reader
does not treat a transient draw-item address as persistent geometry identity.

There are three coverage states:

| State | Consumer behavior |
| --- | --- |
| Live | Read the four stored corners and constant raster-depth bound |
| Empty | Preserve the established collapsed implicit proxy |
| Unavailable, missing or mismatched | Execute the original coverage/depth expressions |

Only existing coverage/policy rejection produces an empty entry. Nonfinite
results, unsupported inputs, missing buffers, failed PSO/SRG preparation or
ambiguous views retain the original path. Count zero in the raster SRG disables
both cache lookups. This makes fallback a reference computation rather than a
new culling decision.

Local geometry, topology, packed ordering or baked uniform scale changes
publish a new matching coefficient buffer. Rigid motion retains geometry-only
coefficients and prepares fresh view results. Camera/projection/jitter, profile,
LOD/proxy selection and render-view changes are naturally reflected by the next
frame-local preparation. The current generation architecture may rebuild
coefficients along with a complete generation even when a change affects only
profile inputs; there is no cross-generation coefficient-reuse claim.

## Numerical and coverage contract

`ImplicitProxyCoverage.azsli` moves the existing implicit algorithm as a unit:
the six affine, unnormalized displaced endpoints, projection and reverse-Z
clipping, all 15 endpoint-pair intersections when needed, optional LOD cut,
padding/clamping, tighter-triangle eligibility and oriented-wall candidate
order/winding. The cache stores the four final corners and preserves the
existing index topology. Vertices do not reconstruct an approximate rectangle
or triangle from compressed shape parameters.

The reverse-Z raster bound is calculated independently of the selected
coverage shape from the expanded ray-test enclosure. The existing cheap bound,
frustum candidate enumeration, residual/error authorization and fallback to
`1` remain intact:

```
actual written hit depth <= cached raster bound <= 1
```

An uncertain certificate does not establish empty coverage. The pixel shader
still evaluates the actual winning hit and writes its depth. MSAA hit records,
gradient sidecars, repair policy and depth-equal forward shading remain
sample-specific. Batch resolve rectangles still cover the broader producer and
repair support; the implicit near cache does not replace that scheduling bound.

Two numerical details were necessary for fidelity. First, barycentric weights
retain their original inline denominator expression even though identical raw
area bits are stored. Loading the cached area conditionally changed reciprocal
common-subexpression handling in Newton and produced one-ULP UV/depth
differences in 82 comparisons. Preserving the original expression graph removed
those differences while retaining cached partials and tolerance; the coefficient
path adds no global math-mode or Newton `precise` change.

Second, the final computed NDC corners are explicitly `precise`. The original
vertex path's `precise SV_Position` imposed that dependency on the coverage
expressions. A compute UAV output has no `SV_Position`; carrying the same
precision contract on the final NDC values preserves the original arithmetic
through the moved coverage calculation. This is separate from the coefficient
expression-graph decision.

## Controls and validation status

`r_silpomFaceCache` defaults to true. Set it to false to retain the reference
expressions, without changing the geometry, resolve-rectangle setting or MSAA
policy. `r_silpomFaceCacheTrace 1` emits preparation and raster-binding traces.
Active validation must observe nonzero `batches`/`faces`, `output=ready`,
`table=ready` and matching `FaceCacheBind ... resources=ready` records for the
actual view/tick. Reference runs should report count zero/reference bindings.
The logged coefficient-face count is dispatch scope, not a GPU readback of how
many version-guarded coefficient writes occurred.

Focused coefficient verification passed 82,801 CPU checks and the SM6.2/SM6.7
GPU compare, all-records-missing and disabled modes with zero finite-result
bit mismatches. The frozen tower records are export-derived FP32 inputs, not a
live descriptor-buffer dump. The four coverage/depth GPU tests exercise normal
and count-zero fallback modes at SM6.2 and SM6.7. Each passed 2,048 face/view
cases, including 1,063 collapsed results and 8,192 corner comparisons, with zero
geometry or depth mismatches.
Detailed evidence is in `scratch/face-cache-20261117/proof-restored-initial.json`,
`coverage-shader-proof.json` and `focused-restoration-shader-proof.json`.
The first shader/coverage proof is reusable because all 18 source hashes were
restored exactly; all eight coefficient-test DXIL artifacts are also byte-identical.

## Runtime results and numeric policy

Five views at 1280x720 and 2x MSAA cover 5,557,447 owned shaded samples. Each
view alternated cache-off/full-screen, cache-on/disjoint rectangles, and
cache-off/repeated-full. Every enabled run has same-frame preparation, hit-depth
and forward binding evidence. All 70 current off/on/off attachment comparisons
are bit-exact. The temporary controls were restored and the saved level was
unchanged.

Against the preceding prepared-normal renderer, depth, ownership, status,
representation/validity metadata and all owned diffuse/specular/normal outputs
are bit-exact. One lower-glancing sample (x=731, y=308, MSAA0) has a two-ULP U
change (absolute 1.4901161193847656e-8). Related UV-gradient and depth-gradient
components at that same sample differ by at most two ULPs (absolute maxima
5.820766091346741e-11 and 1.1641532182693481e-10). Four other views are entirely
bit-exact. Thus this is bounded numerical equivalence, not bitwise hit/gradient
equality. No depth or shaded-output tolerance is applied.

The strict failure is preserved in `independent-review-regression.json`. A
broader Newton fallback rewrite increased differences and was discarded. The
first smaller implementation was restored byte-for-byte, and its original
105 attachment captures were independently rechecked with an explicit two-ULP
UV/gradient bound. The comparison utility defaults to strict zero tolerance;
`test_face_cache_validation.py --uv-roundoff-ulps 2` explicitly selects the
reported policy. Nonfinite values, changed metadata, changed clears, or larger
roundoff fail. The separate bounded audit is
`scratch/face-cache-20261117/independent-bounded-initial.json`.

This evidence comes from the original captured build, restored exactly. It is
not a fresh post-restoration scene capture. The independent identity check
verified 266 renderer/native/fixture files, all 37 recorded capture-source
hashes, the 18 shader sources and eight coefficient DXIL artifacts. Two test
scripts changed to record scope and select explicit comparison policy. The
original snapshot did not hash every asset-cache product, external engine
binary or driver file; no changed recorded dependency invalidated capture
reuse. Morph, learned proxy-depth, diagnostic and copied-donor repair samples
were not selected by these views; their separate compile/math checks do not
constitute runtime coverage.

No GPU timing or speedup is claimed. Preparation dispatch, allocation, table
setup and buffer traffic must be measured with their consumers before drawing
a performance conclusion.

The restored native build linked successfully. Its final asset processing completed
640 jobs with zero failed jobs or processing errors; readiness found no stale
products. All 268 controlled inputs/binaries remained unchanged through this
final rebuild. The frozen summary and evidence hashes are in
[FaceViewCache20261117.json](../Tests/Reference/FaceViewCache20261117.json).

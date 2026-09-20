# FBX planar face experiment

This experiment applies the existing planar heightfield renderer to tagged FBX
triangles, with a restricted heuristic for blending displacement directions
across tagged edges. Base triangles stay planar. There is no arbitrary authored
curved-surface support, compute hit buffer or hardware ray tracing.

One SilPOM Mesh component owns ordinary geometry and tagged face batches.
Face tags are independent of material assignments. Tagged base triangles are
absent from the component's ordinary geometry. Do not also attach the stock FBX
model: Asset Processor still emits that separate, unfiltered model product.

The renderer batches by material/profile, with one 144-byte descriptor and four
coverage vertices per tagged triangle. It does not create per-triangle components
or materials. Imported UVs define the affine mapping on each triangle. Rays are
clipped to the triangle's extruded UV domain on the constant-direction fast path.
Varying directions use planar-kernel seeds and bounded local refinement; only
converged roots inside the triangle are accepted. Color and camera depth use
that solver; shadow maps rasterize the original triangles. The existing
SilPOM Patch component and its flat-shadow policy are unchanged.

## Artist workflow

1. Package the add-on with `Tools/package_blender_addon.py`, then install its ZIP
   in Blender. In Edit Mode, use the SilPOM Faces panel to assign a region/profile
   to selected faces, or mark faces ordinary. Material slots do not select faces.
2. Export the FBX and `.silpom.json` together into a project asset directory.
   Continue authoring from the exported authoring copy to retain identities.
3. Build SilPOM.Builder, SilPOM.Editor and SilPOM; let Asset Processor finish.
4. Add one SilPOM Mesh component, select the `.fbx.silpommesh` product, and assign
   its discovered source materials and height profile. Use opaque StandardPBR.
5. Use DX12 and set `r_meshInstancingEnabled false` in this experimental session.
   The capture harness uses single-sample rendering. Broader backend/AA support
   has not been accepted.

The height must be normalized, linear, single-mip R32_FLOAT/R16_UNORM/R8_UNORM.
Profile amplitude is in world metres, independent of positive uniform entity
scale. Repeat/clamp and signed UV tiling are supported. Degenerate UV triangles
are rejected. The initial export contract excludes modifiers, shape keys,
nonuniform/negative scale and deforming meshes.

## Expected edge behavior

- The builder joins corner fans through tagged/tagged edges using logical vertex
  IDs. UV seams, material splits, region IDs and profile IDs do not split normals.
  Faces touching only at a vertex remain separate fans. Ordinary faces do not
  contribute to a tagged fan. Corner angles weight geometric face normals, so
  triangulating a flat quad does not bias the normal toward its diagonal.
- Shared corners receive identical unit directions. Displacement interpolates
  these vectors linearly **without renormalizing**. Matching heights therefore
  produce identical shared-edge positions. Interior displacement magnitude can
  be slightly smaller than the profile amplitude as the interpolated vector's
  length decreases. The six displaced corner extrema conservatively bound it.
- Lighting normalizes the interpolated base normal and rotates the heightfield's
  relief normal into that frame. This smooths the base fold; heightmap gradient
  discontinuities, differing UV mappings or materials can still create a shading
  seam. Smooth lighting is not the exact geometric normal of the displaced fold.
- Normal blending cannot reconcile mismatched height samples/profiles. These
  still need compatible UV/height values for a watertight displaced edge.
- A tagged/ordinary boundary can reveal an open step or gap.
- No caps, bevels, tapering or invented connecting geometry are generated.
- Magenta means exhausted traversal; yellow means invalid input. These are
  diagnostics, not acceptable edge artifacts or valid surface hits.

## Normal-blending implementation and limits

`Include/SilPOM/FaceNormals.h` implements the import heuristic. Nonmanifold or
inconsistently wound edges are rejected. Cancelling normals and tagged fans
whose generated direction has a dot product below 0.25 with an incident face
normal are rejected rather than silently introducing another hard boundary.
The builder fingerprint and surface contract are version 2; existing FBX pairs
need asset reprocessing, not a new export.

`Assets/Shaders/SilPOM/BlendedFace.azsli` defines the narrow surface model
`P(u,v) + h(u,v) D(u,v)`, with affine base position and affine generated direction.
Faces with constant directions retain the planar solver. Other faces try the
mean and three corner directions as planar seeds, at most eight candidates per
seed and sixteen Newton steps per candidate. Planar traversal shares the profile
cell budget across seeds. Unconverged iterates are never returned as valid hits.

This local refinement is experimental: it does **not** prove nearest-root
completeness for grazing rays, large displacement or self-overlapping surfaces.
It can miss a root without finding a seed. A rejected refinement seed contributes
no geometry; the remaining candidates are still tried. Actual traversal/candidate
budget exhaustion retains the magenta camera diagnostic. Errors never write
shadow depth. Four seeds reduce missed-root problems but do not remove the
limitation. This is not the previous general curved solver.
Original standalone planar behavior and its conformance tests remain unchanged.

## Reproduction

Run Blender in background with `Tests/create_planar_mesh_fixture.py -- --output
<project>/Assets/SilPOMPlanarFaces`, using a new output directory. The fixture is
a folded wall with eight triangles: six tagged, two ordinary, and two materials.
The red material is shared between tagged and ordinary faces. A 64x64 periodic
heightfield crosses the coplanar diagonal, fold and tagged/ordinary boundary.

Process with AssetProcessorBatch and `Tests/planar_dx12.setreg`, then launch a
separate Editor with `--rhi=dx12 --project-path=<project> --runpython
<absolute-path>/Tests/planar_mesh_editor_smoke.py`.
The script builds an unsaved fixture in DefaultLevel, captures flat/displaced,
fold, grazing, off-centre and near-plane views at 1280x720, checks activation
and invalid-profile retention, and leaves the fixture open. It never saves the
level. `SILPOM_FACE_EXIT=1` exits after testing. Captures and result.json go to
`<project>/user/SilPOMPlanarFaces`. Set `SILPOM_FACE_OUTPUT=SilPOMBlendedFaces`
to retain a separate capture set when comparing against the baseline.
If the level already contains a saved `SilPOM_Planar_FBX_Edge_Study`, the harness
replaces that named entity only in its unsaved session to prevent overlapping
copies. It does not change the saved level file.

This is a visual feasibility experiment, not completion of every requirement in
issue #5. Independent shadow/depth reference comparisons, production reimport
dependencies, game-launcher acceptance and realistic scene benchmarks remain
separate follow-up work.

## Fixed-normal baseline result — 2026-09-20, commit 6952051

O3DE 2.7.0, DX12, Radeon RX 7900 XTX, 1280x720, one sample. Builder, Editor
and runtime Gem targets built. The fixture's FBX/sidecar and all material shader
variants processed. The batch still reports one unrelated legacy transport-v1
fixture failure in Assets/SilPOMRoundTrip; that source was left untouched.

The live Editor reported three batches, six planar tagged triangles and 576
descriptor bytes. Seven captures were inspected, including the same camera with
zero and nonzero displacement. The lower-left red area remains ordinary; the
upper red and blue areas show relief and displaced outer silhouettes. No obvious
coplanar triangulation crack or magenta/yellow failure surface appeared in these
captures. Close views expose the 64x64 heightfield's cell-normal faceting and the
unsealed transition from displaced to ordinary geometry. Folded faces retain
independent displacement directions; this experiment does not join their edges.
The near-plane view intentionally cuts through the relief and exposes background.
The capture named viewport_edge.png in this run is only an off-centre view, not
a complete viewport-clipping regression.

Three deactivate/reactivate cycles passed; an invalid amplitude was rejected and
a subsequent valid edit recovered. Core planar, GPU and PlanarFaces CTest suites
passed, along with all eight planar analyzer tests. The new boundary test checks
20,000 analytical rays against a plane/barycentric oracle, both UV windings and
shared/exterior edges. The Windows sidecar staging fix was checked by exporting
under the sandbox account and reading the result under the artist account.

The fixture is left open in an unsaved Editor session. These captures are visual
evidence of the requested planar-face workflow, not independent numerical proof
of every rendered depth/shadow sample or a game-runtime acceptance test.

## Blended-normal result — 2026-09-20

The builder, Editor and runtime rebuilt successfully. Asset Processor rebuilt
the fixture and all 15 shader variants; the same unrelated legacy transport-v1
source remains its single failed asset. The Editor reports three batches, six
tagged triangles and 864 descriptor bytes. Three activation cycles and invalid
profile retention passed. The capture harness's `passed` flag covers component
lifecycle and screenshot capture, not pixel correctness.

All four CTest suites pass: Core, PlanarFaces, FaceNormals and the existing planar
GPU conformance test. New CPU tests execute the shared refinement shader code,
checking angle weights, disconnected fans, tagged/ordinary boundaries, duplicated
UV corners, nonmanifold rejection, shared displaced edge positions, identical
shared-edge ray depths and flat-height lighting normals, analytic constant-height
planes, and known varying-height hits at frontal and grazing ray angles. A DXC
compute compilation also checked the new kernel; it is not a GPU numerical
conformance test for the blended extension.

Seven captures in `<project>/user/SilPOMBlendedFaces` were inspected. The fold
uses shared displacement directions and smooth base shading; the ordinary lower
left remains flat. **Visual acceptance is incomplete:** sparse magenta refinement
failures appear along some relief silhouettes in the close, oblique and grazing
views, and a larger magenta band appears in the near-plane view. These are known
intersection-heuristic failures, not successful hits or normal-blending seams.
The tagged/ordinary boundary remains unsealed by design. The updated fixture is
left open for inspection, and the original fixed-normal captures are preserved.

## Contour regression fix — 2026-09-20

The magenta contours were rejected planar seeds being misclassified as exhausted
surface queries and drawn at the bounding-box entry depth. A 256x144 CPU scan of
oblique and near-plane views reproduced 394 such rays. All missed a separately
tessellated reference surface; none needed a physical surface hit. Distinguishing
seed rejection from actual work-budget exhaustion removed all 394 diagnostics
without changing either view's valid-hit count (3,868 and 36,865 per-face hits).
The shared directions, displacement model and successful root calculations are
unchanged. The subsequent flat-shadow change removes intersection diagnostics
from the shadow pass entirely.

`SilPOM.BlendedArtifacts` adds six permanent oblique/near-plane repro rays checked
against dense reference triangles, plus checks that actual traversal exhaustion
still reports exhaustion. All five CTest suites pass. The updated close-up,
grazing and near-plane captures in `user/SilPOMArtifactStudy2` have no observed
magenta contours. General nearest-root completeness remains outside this test.

## Flat external shadows and local relief shadows

Tagged faces cast their original base triangles into every shadow map. The
coverage quad becomes one triangle plus one degenerate triangle in the shadow
vertex shader. The shadow pixel shader preserves raster depth; it does not
include the heightfield traversal or sample the height texture. Camera depth,
silhouettes and the accepted blended seam still use the displaced intersection.

`SilPOM Mesh > Relief shadow steps` defaults to 16, accepts 0–64 and uses 0 to
disable local relief shadows. Directional lights evaluate a bounded texture
march at each visible SilPOM pixel. The visibility factor attenuates that light's
diffuse and specular response, leaving ambient, emissive and other light types
alone. The normal engine shadow option also disables the local march.

The sunlight shadow-map receiver advances along the light ray to the base plane
when the visible relief is recessed. It samples the directional shadow map
directly, because the engine's fullscreen shadow result uses displaced camera
depth and would falsely shadow those valleys behind the flat proxy. Other
objects and the mesh's larger folds still cast normal flat shadows.

This local relief approximation freezes the interpolated displacement direction
at the hit, samples up to one height texel per step and stops above the maximum
height. It continues the current texture over triangle boundaries to avoid
internal triangulation seams. It does not trace neighbouring faces, blend
different profiles, or implement arbitrary curved surfaces. Near open edges or
profile discontinuities the continued texture can overestimate occlusion; thin
blockers or blockers outside the finite sample radius can be missed. Point and
spot lights retain the engine's ordinary shadow receiving and do not yet get
local relief shadows or the directional receiver adjustment.

Flat casting removes all per-shadow-pixel relief queries. Local relief shadows
add bounded work per visible pixel and directional light, plus direct shadow-map
sampling instead of the fullscreen result. No frame-time improvement is claimed
without a scene benchmark. `SilPOM.ReliefShadows` checks ridge occlusion, overhead
lighting, disabled/zero-amplitude no-read paths, repeat/clamp, signed displacement,
finite budgets and the recessed-receiver adjustment. The Editor harness captures
the same oblique view with local shadows on/off and with zero displacement for
external-shadow comparison.

Validation on 2026-09-20: runtime, Builder and Editor targets built; all six
CTest suites passed. All 15 material shader variants processed, with only the
pre-existing `Assets/SilPOMRoundTrip` transport-v1 fixture failure remaining.
The generated DX12 shadow shader contains neither `SilPomIntersect` nor
`SilPomTexel`; the forward shader contains both the relief march and receiver
adjustment. Ten captures and three activation cycles completed in
`user/SilPOMFlatReliefShadows`. Inspected on/off captures show local shadowing on
the tagged faces; close-up and near-plane views preserve the seam without
magenta contours. Terrain speckles remain with zero displacement and are not
claimed fixed by this change.

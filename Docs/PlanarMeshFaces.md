# FBX planar face experiment

This experiment applies the existing planar heightfield renderer to tagged FBX
triangles. A mesh can contain faces at different angles. Each triangle displaces
along its fixed geometric face normal; there is no interpolated displacement
direction, curved-surface solver, compute hit buffer or hardware ray tracing.
The transported corner shading normals/directions do not bend the heightfield.

One SilPOM Mesh component owns ordinary geometry and tagged face batches.
Face tags are independent of material assignments. Tagged base triangles are
absent from the component's ordinary geometry. Do not also attach the stock FBX
model: Asset Processor still emits that separate, unfiltered model product.

The renderer batches by material/profile, with one 96-byte descriptor and four
coverage vertices per tagged triangle. It does not create per-triangle components
or materials. Imported UVs define the affine mapping on each triangle. Rays are
clipped to the triangle's extruded UV domain before invoking Heightfield.azsli,
so the surrounding coverage rectangle cannot become visible geometry.
The same planar kernel is used for color, depth and shadow variants. The existing
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

- Coplanar triangles with matching UVs/profile describe the same surface along
  their shared edge, including the triangulation diagonal of a planar quad.
- At a fold, adjacent faces displace in different directions: gaps and overlaps
  are possible. This is the behavior the experiment is intended to inspect.
- A tagged/ordinary boundary can reveal an open step or gap.
- No caps, bevels, tapering or invented connecting geometry are generated.
- Magenta means exhausted traversal; yellow means invalid input. These are
  diagnostics, not acceptable edge artifacts or valid surface hits.

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
`<project>/user/SilPOMPlanarFaces`.

This is a visual feasibility experiment, not completion of every requirement in
issue #5. Independent shadow/depth reference comparisons, production reimport
dependencies, game-launcher acceptance and realistic scene benchmarks remain
separate follow-up work.

## Local result — 2026-09-20

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

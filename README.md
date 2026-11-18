# SilPOM

An optional O3DE Atom Gem for finite, static, planar displacement patches.
An [FBX planar-face experiment](Docs/PlanarMeshFaces.md) also applies the existing
solver to tagged mesh faces through one SilPOM Mesh component, with open edges
for visual inspection. It does not use the preserved curved-surface solver.
Developed for [TG #40](https://github.com/brooteskan/TG/issues/40).
See the [validation record](Docs/Validation.md) for tested behavior and remaining
release-hardening work.
See the [material initialization regression](Docs/MaterialInitialization.md) for
the mesh/patch PSO warning fix and its editor reproduction.
See the [StoneWall pixel shader performance follow-up](Docs/MeshPixelPerformance.md)
for the exact texel-addressing optimization, GPU measurements and image validation.

The raster adapter draws conservative coverage and intersects a bounded bilinear
heightfield per sample. It shades and writes depth at the actual intersection;
misses reveal the background. Camera depth and color use the same intersection
implementation. Shadows use the original flat quad, with **no heightfield
intersection**. Expanded camera coverage is never a shadow caster. Coverage does
not depend on off-screen image data. See [planar A/B testing](Docs/PlanarAB.md) for
the issue #3 comparison harness and its acceptance limitations.

## Engine requirement

SilPOM needs O3DE 2.7.0 (`061180bf`) with the engine patch bundle from
[brooteskan/o3de-patches](https://github.com/brooteskan/o3de-patches) applied;
a stock engine does not build it. The tested revision and the reasons are in
[Integration](Integration/README.md).

## Add to an O3DE project

1. Clone this repository into your project (for example `Gems/silpom`), or
   anywhere and register it as an external subdirectory.
2. Add the gem's folder to `external_subdirectories` and `SilPOM` to `gem_names`
   in the project's `project.json`.
3. Configure/build the project, including `SilPOM` and `SilPOM.Editor`, then let
   Asset Processor finish processing the new material type and shaders.
4. Make a material using `Materials/Types/SilPOM/SilPOM.materialtype`, assign its
   `surface.heightMap`, and assign the material to a **SilPOM Patch** component.
5. Author the patch in local XY with displacement along local +Z. Width and
   height are metres before positive uniform entity scale. Displacement amplitude
   remains in world metres. Rotate the entity to orient a wall.

Use normalized linear scalar height with one mip (R32_FLOAT, R16_UNORM or
R8_UNORM). Height geometry uses explicit bilinear texel reads; ordinary averaged
mips cannot represent the same surface. Use a single-mip LUT image preset.
Color, normal and AO maps use ordinary filtered samples at the hit UV.

SilPOM Patch and SilPOM Mesh surfaces receive Atom Decal components in the main
forward pipeline. Assign a decal material with base-color and/or normal textures;
the component's Opacity and Normal Map Opacity control the two blends separately.
Projection uses the displaced surface position. Decal normals feed both direct
and indirect lighting, and decals leave the heightfield geometry unchanged.

## Test project

`Tests/Fixtures/TowerBenchmark` is a complete O3DE project (`SilPOMTowerTest`)
that loads this gem from `../../..` and holds the tower benchmark level and
every asset it uses. Editor harnesses under `Tests/` open its
`Levels/SilPOMTowerBenchmark` level unless `SILPOM_TEST_LEVEL` names another,
and the `Tests/Run-*.ps1` runners default to it (`-ProjectPath`, `-EnginePath`
and `-BinaryPath` select others). Offline tools under `Tools/` read their
inputs from it unless given `--project`. See its
[README](Tests/Fixtures/TowerBenchmark/README.md).

`Tests/editor_smoke.py` creates an unsaved wall fixture, captures front/grazing/
inside views, checks component lifecycle and exits. First create its materials
(by default from the fixture's fieldstone wall textures; pass `--project` and
`--height/--basecolor/--normal` for another project's textures):

```powershell
python Tools/prepare_wall_fixture.py
```

Then process assets and launch a separate Editor session with
`--project-path=<gem>/Tests/Fixtures/TowerBenchmark --runpython <absolute-path-to-Tests/editor_smoke.py>`.
Captures and the JSON result go to the project's `user/SilPOMValidation` directory,
or to `SILPOM_CAPTURE_DIR` if set. The script does not save DefaultLevel.

## Surface contract and ray tracing

`Assets/Shaders/SilPOM/Heightfield.azsli` contains the shared intersection kernel.
It traverses texel cells in ray order and solves each bilinear cell analytically.
The result distinguishes hit, miss, budget exhaustion and invalid input.
Magenta indicates exhausted traversal; yellow indicates invalid input. These
override optional debug views (1 normal, 2 cell-budget heatmap, 3 UV, 4 depth,
5 hit mask). Increase the budget or correct invalid inputs before accepting a
scene. View 0 is ordinary material shading with failure colors still enabled.

The portable descriptor and optional Atom procedural intersection shader allow
future BLAS/TLAS integration without changing the authored surface. Native DXR
conformance tests already exercise the same kernel. Production Atom RT effects
are **not enabled**: proxy triangles are excluded from ray tracing to prevent
false flat occlusion. The optional intersection adapter is DX12-only; the current
engine Vulkan DXC crashes compiling it. See [architecture](Docs/Architecture.md)
for resource lifetime, descriptor versioning and conservative acceleration rules.

## Standalone conformance tests

```powershell
cmake -S . -B build/core
cmake --build build/core --config Release
ctest --test-dir build/core -C Release --output-on-failure
```

On Windows, configure with `-DSILPOM_DXC=<absolute-path-to-dxc.exe>` to additionally
build native D3D12 compute tests without requiring DXR. Add
`-DSILPOM_DXR_TESTS=ON` only for the separate optional procedural DXR test.
The planar raster path does not build or invoke ray-tracing acceleration
structures. CPU tests compare the shared shader
kernel against an independent double-precision oracle, including finite ray
intervals, tangent hits, camera entry, UV seams and signed displacement.

## v0 limits

Static patches only; no skinned or arbitrary curved meshes, collision changes,
overhangs, closed solids or runtime animated transforms. Author backing/caps as
ordinary geometry. Only positive uniform entity scale is supported. The stock
proxy motion pass is suppressed; static camera motion uses the true depth buffer.
Reflection probe capture and production RT effects are excluded. Keep the
material opaque. No performance advantage is assumed; grazing rays and large
coverage rectangles can be expensive without conservative acceleration.

MIT licensed.

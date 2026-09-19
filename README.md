# SilPOM

An optional O3DE Atom Gem for finite, static, planar displacement patches.
Developed for [TG #40](https://github.com/brooteskan/TG/issues/40).
See the [validation record](Docs/Validation.md) for tested behavior and remaining
release-hardening work.

The raster adapter draws conservative coverage and intersects a bounded bilinear
heightfield per sample. It shades and writes depth at the actual intersection;
misses reveal the background. Camera depth, color and shadow passes use the same
intersection implementation. Coverage does not depend on off-screen image data.

## Add to an O3DE project

1. Clone this repository into `Gems/silpom` in your project.
2. Add `Gems/silpom` to `external_subdirectories` and `SilPOM` to `gem_names` in
   the project's `project.json`.
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

`Tests/editor_smoke.py` creates an unsaved wall fixture, captures front/grazing/
inside views, checks component lifecycle and exits. For TG's existing StoneWall
textures, first run:

```powershell
python Tools/prepare_tg_fixture.py --project D:/TG/TGProject
```

Then process assets and launch a separate Editor session with
`--project-path=D:/TG/TGProject --runpython <absolute-path-to-Tests/editor_smoke.py>`.
Captures and the JSON result go to the project's `user/SilPOMValidation` directory,
or to `SILPOM_CAPTURE_DIR` if set. The script does not save DefaultLevel.

## Surface contract and ray tracing

`Assets/Shaders/SilPOM/Heightfield.azsli` contains the shared intersection kernel.
It traverses texel cells in ray order and solves each bilinear cell analytically.
The result distinguishes hit, miss, budget exhaustion and invalid input.
Magenta indicates raster traversal failure; increase the cell budget or reduce
the footprint complexity before accepting a scene.

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
build native D3D12 compute and procedural DXR tests. The GPU test explicitly
reports when ray tracing is unsupported. CPU tests compare the shared shader
kernel against an independent double-precision oracle, including finite ray
intervals, tangent hits, camera entry, UV seams and signed displacement.

The standalone build also contains the research-only curved-mesh feasibility
targets `SilPOM.CurvedCPU` and a curved compute phase in `SilPOM.GPU`. They do not
change the runtime component or asset ABI. See
[`Docs/CurvedIntersectionFeasibility.md`](Docs/CurvedIntersectionFeasibility.md)
for the surface contract, measured costs, explicit failure semantics, and the
remaining proof gates before curved asset/runtime work may begin.

The Blender-to-O3DE export/import fixture is documented in
[`Docs/ExportImportRoundTrip.md`](Docs/ExportImportRoundTrip.md), including captured
baseline data, validation and a same-path reordered reimport runner.
It now also documents `Tools/export_mesh.py` for existing authored meshes,
persistent identities, varying displacement fields, and multi-object validation.
See [traversal experiments](Docs/CurvedTraversalExperiments.md) for measured compact
representations, GPU attribute validation, and the remaining coverage/performance
gates. These tools do **not** yet enable curved rendering in the runtime Gem.

## v0 limits

Static patches only; no skinned or arbitrary curved meshes, collision changes,
overhangs, closed solids or runtime animated transforms. Author backing/caps as
ordinary geometry. Only positive uniform entity scale is supported. The stock
proxy motion pass is suppressed; static camera motion uses the true depth buffer.
Reflection probe capture and production RT effects are excluded. Keep the
material opaque. No performance advantage is assumed; grazing rays and large
coverage rectangles can be expensive without conservative acceleration.

MIT licensed.

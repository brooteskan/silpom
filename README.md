# SilPOM

An optional O3DE Atom Gem for finite, static, planar displacement patches.
Developed for [TG #40](https://github.com/brooteskan/TG/issues/40).
See the [validation record](Docs/Validation.md) for tested behavior and remaining
release-hardening work.

The new imported-mesh path is experimental and is not yet issue #2 delivery.
It now uses staged compute intersection/resolution with shared hit/depth records
and thin material shaders, rather than compiling the solver into pixel shaders.
See [imported mesh integration](Docs/ImportedMeshIntegration.md) for its current
resource path, small-fixture reproduction and explicit remaining limits.

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

## Install and run the Blender exporter

The **Curved SilPOM Exporter** add-on has been validated with **Blender 5.0.1**.
It bundles the same exporter used by the command-line tools; Blender does not
need a repository path or additional Python packages after installation.

1. Build the installable ZIP from the gem directory
   (`D:/TG/TGProject/Gems/silpom` for TG), using Python 3:

   ```powershell
   python Tools/package_blender_addon.py
   ```

   If Python is not on PATH, use Blender's bundled interpreter:

   ```powershell
   & 'C:/Program Files/Blender Foundation/Blender 5.0/5.0/python/bin/python.exe' Tools/package_blender_addon.py
   ```

2. In Blender, open **Edit > Preferences > Add-ons**, open the menu at the top
   right, and choose **Install from Disk**. Select
   `build/addon/silpom_exporter.zip` without unzipping it, then enable
   **Curved SilPOM Exporter**. This uses Blender's supported
   [legacy add-on ZIP installation](https://docs.blender.org/manual/en/5.0/editors/preferences/addons.html#installing-legacy-add-ons).
   Rebuild and reinstall the ZIP after updating the exporter.
3. Prepare a local static mesh with an active UV map and a material on every
   face. Apply active modifiers, remove shape keys, and use positive uniform
   object scale. Linked-library meshes and sheared transforms are unsupported.
4. In the 3D View, press **N**, open **SilPOM**, and enter **Edit Mode**. Select
   the faces to displace, choose a **Region** and **Profile**, and click
   **Assign to Selected Faces**. Untagged faces on that mesh remain ordinary.
   Use **Mark Selected Faces Ordinary** to clear displacement from faces, or on
   all faces of an entirely ordinary mesh that you want to include in the export.
   Region/profile attributes are created automatically; tagging supports
   multi-object Edit Mode and Undo. Profiles are numeric references, not height
   map assignments. Directions default to shading normals; see the
   [authoring contract](Docs/ExportImportRoundTrip.md#general-authoring-exporter-transport-version-2)
   for explicit direction fields.
5. Choose **File > Export > Curved SilPOM (.fbx + .json)**, or click the panel's
   export button. Choose a filename such as `wall.fbx`. **Selected Objects Only**
   is enabled by default; disable it to export every scene mesh. Every included
   mesh needs region tags, UVs and materials. Leave **Initialize / Repair IDs**
   enabled to allocate missing identities while preserving existing valid IDs.

Export writes `wall.fbx`, `wall.silpom.json`, and `wall.authoring.blend` together.
Keep the FBX and sidecar paired. The current scene retains its selection, mode,
units and authored geometry; temporary export data is removed even on failure.
Successful exports retain persistent IDs in the open scene. Save your working
file normally to keep them, or continue from the generated authoring copy.
Export never overwrites the currently open `.blend` file.

Existing bundle files are protected unless **Overwrite Export Files** is enabled.
If working from the generated authoring copy, choose a different export filename
or directory so the new copy does not overwrite the open file.

The original command-line workflow is also available from the gem directory:

```powershell
& 'C:/Program Files/Blender Foundation/Blender 5.0/blender.exe' `
  --background '<source.blend>' --python-exit-code 1 `
  --python Tools/export_mesh.py -- `
  --output build/my-export --initialize-identities
```

Unlike the add-on's default, this command exports **all scene mesh objects**. Add
`--object 'MyMesh'` after `--` to restrict export; repeat it for multiple objects.
The output contains `roundtrip.fbx`, its matching `roundtrip.silpom.json` sidecar,
and an `authoring.blend` copy with persistent identities. Keep the FBX and sidecar
together. Ordinary Blender FBX export does not create the required metadata.

For command-line use, continue editing the generated `authoring.blend`. On
subsequent exports, use that copy as the input, omit
`--initialize-identities`, and choose a new output directory (for example,
`build/my-export-next`) so the input authoring file is not overwritten.

For the current TG/O3DE import-and-validation workflow, follow the
[round-trip instructions](Docs/ExportImportRoundTrip.md#general-authoring-exporter-transport-version-2)
using `Tools/run_roundtrip.py --blend <authoring.blend>` with the documented
Blender, Asset Processor and project arguments. The runner overwrites its reserved
test assets in `Assets/SilPOMAuthoredRoundTrip` and checks the imported metadata
and stock mesh product. This is an experimental data round trip, **not curved
displacement rendering**; the runtime Gem still supports planar patches only.
The [add-on validation instructions](Docs/ExportImportRoundTrip.md#blender-add-on)
cover installation, operator regressions and optional automated Blender tests.

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
The [coverage follow-up](Docs/CurvedCoverageGate.md) records interval and exact
quadratic certificates, resolved CPU/GPU regressions, boundary sweeps, and
explicit limits that still prevent runtime promotion.

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

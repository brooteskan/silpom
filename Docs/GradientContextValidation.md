# Per-sample gradient validation

`TowerGradientContextValidation.py` captures the corrected renderer separately
from performance measurements. It uses the established tower level and the same
four fixed views: exterior, lower-facing, lower-glancing and upper-glancing.
The viewport is 1280 by 720, MSAA is 2x, and height-derived normals are enabled.
Receiver UV paint is unbound in the fixed views, and the saved material/lighting
configuration is checked before capture. The script does not save the level.

At each view, it captures a full-screen resolve, disjoint rectangles, and a
second full-screen control. It waits for stable rendering after each change.
The inspector exports each MSAA sample into its own slot in a four-slot atlas;
only the first two slots are analyzed. Derivatives remain changes per raster
pixel, with no division by sample count.

Each mode exports these attachments:

| File suffix | Inspector slot | DDS DXGI format |
|---|---|---|
| `hit` | `Records` | R32G32B32A32_UINT (3) |
| `depth` | `SampleDepth` | R32_FLOAT (41) |
| `diffuse` | `DiffuseSamples` | R16G16B16A16_FLOAT (10) |
| `specular` | `SpecularSamples` | R16G16B16A16_FLOAT (10) |
| `normal` | `NormalSamples` | R16G16B16A16_FLOAT (10) |
| `uv-gradients` | `SampleUvGradients` | R32G32B32A32_FLOAT (2) |
| `depth-gradients` | `SampleDepthGradients` | R32G32B32A32_FLOAT (2) |

The two gradient atlases hold source `(du/dx,dv/dx,du/dy,dv/dy)` and recorded
`(dz/dx,dz/dy,representation,validity)`. Representation occupies the low byte;
repair provenance occupies the next byte. The analyzer rejects non-finite or
unbounded owned values, invalid/fractional metadata, unknown representations or
repair tags, zero coarse footprints, and uncleared unowned sidecars. It reports
every representation, repair type and validity count, plus mixed-owner sample
pixels and same-owner pixels whose sample gradients differ. Missing categories
are listed explicitly; their behavior is not claimed to have been exercised.
Every fixed view must contain at least one conditioned owned context, and the
fixed fixture must contain conditioned relief or morph samples. No minimum
fraction is imposed. This prevents an all-coarse viewport initialization failure
from passing solely because its fallback values are finite.

The default final case is an additional wider exterior view. Its horizontal
camera coordinates are doubled without changing height or orientation. With
5B depth disabled, the 5A error budget set to its supported maximum of four
pixels and relief LOD budget set to 64, this probes authored-plane proxies and
ordinary shell/base mappings. These are temporary diagnostic controls, recorded
on each run. They are restored before the final fixture checks and again during
error cleanup. Set `TG_SILPOM_GRADIENT_DIAGNOSTIC=0` to omit this case. Repair,
morph and backup coverage depends on the actual selected samples; use the
representation counts to identify gaps requiring separate fixtures or solver
tests.

`TG_SILPOM_GRADIENT_PAINT_PROBE=1` is also enabled by default. This adds one
painted lower-facing case after the fixed and wider cases; set the variable to
zero to omit it. The wider overrides are restored first. A fresh unpainted hit
readback selects a visible, well-contained profile-1 face and source UV. An
unsaved entity receives a Surface Decal Paint component, two anchored fixed
dabs and two anchored live dabs, using the existing processed hard-round brush.
A 64 by 64 receiver UV chart is enabled in atlas mode. No benchmark reference
mesh, replacement source asset or saved paint document is created.

The probe waits for a bound, ready atlas, two fixed dabs, two live dabs and
active projection batches. It records the real paint document and status, and
exports the same seven attachments in full/rectangle/control order plus the
per-sample UV usage atlas. A pristine hit/depth/color/normal control establishes
unchanged surface samples and visible paint. A final prefix-only color/normal control
keeps the atlas while removing just the live tail. Analysis requires nonzero
atlas usage, a visible fixed-prefix contribution on atlas samples, and a visible
live-tail contribution. It does not claim an ordinary non-composite decal was
rendered. The hard-round brush uses the engine's constant default normal image,
so this probe does not establish filtering quality for a spatially varying
paint normal texture.

The pristine paint control precedes creation of the temporary entity. Editor
prefab propagation can publish a fresh mesh generation during that setup.
`Mesh.cpp` allocates runtime hit-face ranges with `AllocateHitFaceBase` in
generation/batch order; those integer addresses are not persistent face IDs.
`BeginReceiverUvPaint` in `ReceiverUvPaint.inl` finds the document's canonical
face string in the current generation's `paintFaces`, then binds its current
`hitFace`. The canonical face stays the same while its packed runtime address
can change.

Only this pristine-to-painted comparison permits a declared address remap.
The offset comes from the captured original face's `hit_face` and the bound
receiver `hit_face`, which must be identical across full/rectangle/control.
The document's `ReceiverUv.Face` must match the captured canonical face string.
The analyzer requires every owned pixel address to shift by precisely that
declared constant and every background owner to remain zero. It then compares
all four hit words exactly after changing only the packed owner bits, preserving
the low status bits, and requires exact sample depth. No offset is fitted from
image data. Raw changed-hit counts, UV/z/status differences, original/bound IDs
and the remap basis remain in the report. Usage must identify the declared bound
face. Core unpainted historical comparisons and corrected scheduler comparisons
continue to require the original raw hit bits with no remapping.

The receiver binding is released and the temporary entity is deleted before the
final fixture checks. A separate error cleanup attempts this even when the
paint setup, bake or capture fails, without preventing restoration of the
original normals, AA, camera and viewport settings. The optional probe adds
about 1.13 GiB of uncompressed readbacks; its 64-square chart is approximately
155 KiB including all guttered mips. Actual elapsed time depends on readback
and asset readiness. Each bake wait is bounded at 180 seconds.

After the updated engine hooks, native code and shader assets have been built,
run the capture script in a dedicated Editor instance. Choose a fresh output
directory, and use the rebuilt Editor appropriate for this project:

```powershell
$env:TG_SILPOM_GRADIENT_OUTPUT = 'D:/TG/TGProject/user/silpom-gradient-context-2x/quality'
& $editorExe --project-path='D:/TG/TGProject' --runpython 'D:/TG/TGProject/Gems/silpom/Tests/TowerGradientContextValidation.py'
python D:/TG/TGProject/Gems/silpom/Tests/test_tower_gradient_context.py `
    $env:TG_SILPOM_GRADIENT_OUTPUT "$env:TG_SILPOM_GRADIENT_OUTPUT/report.json"
```

The analyzer first requires successful restoration and unchanged saved level.
It requires bit-exact full/rectangle/control equality for hit records, depth
and both gradient atlases over every sample, including sky. Diffuse, specular
and normal attachments must match exactly on all owned samples, including
diagnostic samples. All owned corrected shading and depth values must be finite.
The repeated full-screen control must be stable; scheduling differences are not
accepted as noise.

The fixed views are also compared with the frozen normals-on captures in
`TGProject/user/silpom-multi-rect-2x-20261116/quality`. The analyzer verifies their
published full-screen attachment hashes and stable-control results, and requires
matching scene/configuration hashes and controls. The current view poses must
match the frozen scene fixture. Hit records and depth must remain bit-exact over
all samples. Historical shading is allowed to change because the local filtering
contract changed; the report quantifies changed owned samples, RGB mean/p95/max
absolute differences, RMSE and mean signed change for each shaded attachment.
These are attachment-value differences, not perceptual error or image quality
scores. The wider diagnostic has no historical comparison.

This procedure checks scheduling independence, finite sample contexts and
visibility preservation. It does not independently prove every Jacobian or
synthetic continuation equation. The CPU differential tests and shader compile
checks cover those calculations. It collects no GPU timings, and its readback
cost must not be included in performance captures. No speedup may be inferred
from these results.

The analyzer's lightweight contract tests run without Editor or asset tools:

```powershell
python D:/TG/TGProject/Gems/silpom/Tests/test_tower_gradient_context.py --self-test
```

They exercise sample atlas packing, mixed-owner/provenance counting, rejection
of malformed/non-finite/uncleared contexts, historical shading statistics, and
positive fixed/live contribution checks (including deliberate missing-tail and
missing-prefix failures). Declared generation rebases are checked against valid
uniform mappings and deliberately wrong pixel/offset/status/UV/z/depth/sky data.
Creating these scripts does not constitute an actual tower validation run.

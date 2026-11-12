# Decal Painter

Open **Tools > Decal Painter**, choose **New paint entity**, and enable painting.
The tool is available in any level, including VueLiveLink. It creates a **Decal
Paint** entity with a **Surface Decal Paint** component; it does not alter the
tower's material or its heightmap. Save the level to save your work. Layers and
strokes also travel with the entity when it is saved as a prefab.

## Automatic Composite

Each paint entity retains the **newest 100 dabs** as editable paint. Every older
dab moves into the ongoing **Composite**, including the older section of a long
stroke while the pen is still down. The cutoff counts emitted dabs, not pen
events or complete strokes. Continuous spacing and pressure remain continuous
across the cutoff. Layer trees show their fixed Composite and remaining editable
strokes; the pinned status reports the live and fixed counts.

Composite projections retain their resolved sizes, pressure opacity, colors,
materials, normals and original layer/stroke order. They cannot be individually
edited or undone. Undo/cancel can remove a gesture's remaining live tail; paint
already in Composite remains. Undoing other scene work does not clear its history.
Paint-document commands use the normal Editor undo stack; their snapshots retain
editable data and refer back to the fixed paint archive. Undo and redo therefore
do not depend on old array indices after dabs have moved into Composite.
Layer visibility, opacity and transforms still apply to the whole layer/entity.
Old prefabs migrate automatically on activation; save the level/prefab to persist
the new representation.

The Composite is a compiled **3D projection stream**, not a flat texture bake.
It evaluates paint at the resolved SILPOM surface position and works with ordinary
meshes too. One ordered spatial hierarchy renders the entity's fixed paint and
live tail in **one decal batch**. It skips projections whose bounds do not cover
the shaded point; color-only alpha paint also stops once residual transmittance
falls below 0.00001. Other blend modes and normal paint preserve their full
forward evaluation order. No lighting is baked into the paint.

This removes the former 4096-dab and 256-stroke rendering cutoffs. It does not
turn the older projections into constant-size bitmap storage: composite memory
and rebuilding work grow with the amount of paint. Heavy overlap with non-alpha
blends or normal paint can still be expensive. Layer ordering is defined within
one paint entity, as before.

## Painting

Choose **Paint** or **Erase (selected layer)** in **Brush operation** (the Paint /
Erase selector under the Stamp/Paint/Edit-point selector). Both operations work
with individual stamps and continuous strokes. Erase removes earlier color and
normal paint in the selected layer, including fixed Composite paint, revealing
the material and lower paint layers. Paint added after an erase remains visible.
Opacity, the brush texture's alpha and tablet pressure control erase strength.
Blend, tint and normal-map opacity apply to painting; they are disabled for erase.

The **Heightmap ranges** group has independent **Paint** and **Erase** rows of
**Low**, **Medium** and **High** checkboxes. Select any combination:

- Low: normalized heightmap values below 1/3.
- Medium: values from 1/3 up to 2/3.
- High: values from 2/3 through 1.

All three are selected by default, preserving unrestricted painting and erasing.
Selecting none makes that operation have no effect. A restricted selection affects
SILPOM surfaces with height information; ordinary surfaces without a heightmap
receive the brush only when all three bands are selected. These are heightmap
values, independent of the object's world elevation or height-scale setting.

Each stroke stores its operation and range selection. Selecting a stroke restores
its matching row without changing the other operation's row. **Apply brush
settings to selected stroke** updates its editable tail. Fixed Composite dabs
retain the original operation and height mask, including after saving/reloading.
Erase dabs participate in the same rolling 100-dab limit and undo rules as paint.

Height filtering runs in the shader at the resolved SILPOM hit UV, using the same
bilinear mip-zero height, tiling and repeat/clamp rules as the geometry. It does
not modify the source textures. Restricted batches sample height once per shaded
point, rather than once per dab; unrestricted batches skip that lookup.

- **Stamp:** one decal for each click or pen contact.
- **Paint:** hold the left mouse button or pen tip and drag. Spacing is measured
  along the surface in fractions of the brush diameter, independent of event
  rate. Larger spacing produces fewer dabs and costs less to render.
- Hold **Alt**, or use right/middle mouse, to navigate. Painting only captures
  left-button/pen gestures inside the main level viewport while enabled.
- **Esc** cancels the current gesture and disables painting. **Ctrl+Z/Ctrl+Y**
  use the Editor's undo/redo. One complete gesture is one undo operation.
- Choose the included Soft/Hard round presets, or browse to an existing material
  with a base-color texture containing the decal's alpha. Use the image preset
  **Decal_AlbedoWithOpacity** for custom decal textures. Decal materials need a
  valid normal texture too; the supplied brushes use the engine's flat normal.

Diameter, opacity, normal map opacity, spacing, rotation, projection depth, tint, blend mode and
pressure response become properties of each stroke. To change an existing
stroke, select it, adjust the brush settings, then click **Apply brush settings
to selected stroke**. Changes retain its live dab positions and pressure values.
Only the live tail can be changed. Its point list contains actual dab placements;
spacing controls future dabs and does not reposition existing placements.

## Editing and layers

Select a layer or stroke in the tree. Rename it, toggle visibility, or delete it.
**Lower/Raise** reorder the selected stroke within its layer, or the selected
layer in the stack. Higher items in the stack are drawn later. A layer's opacity
multiplies the opacities of its strokes. Locking a layer prevents painting,
point changes and deletion on it. These actions participate in Editor undo.

To change the path, select a stroke and a point in the point list, then choose
**Edit point**. Click or drag in the viewport to reproject that point onto the
visible surface. The selected point is orange and the remaining path is green.
You can also delete individual points. Gaps remain gaps when their first point
is deleted. The brush outline shows its full, unpressured diameter.

Points are stored in the paint entity's local coordinates. Transform the entity
to move the painting as a group, or parent it to the object it should follow.
A paint entity can hold strokes across several static objects; it is not
automatically attached to whichever object is under each dab. Re-exporting or
changing a target's geometry does not automatically reproject existing strokes.

## SILPOM placement

The brush reads the rendered depth **after** SILPOM's hit, hole-fill and backup
passes, reconstructs the visible position, and estimates its normal from adjacent
depth pixels. It uses the matching frame's jittered projection matrix. Ordinary
meshes use the same path. A moved camera or changed viewport size invalidates the
snapshot until a matching frame arrives. Sky and off-surface excursions break
the stroke instead of joining unrelated surfaces.

This avoids the base-mesh physics collider, which does not describe SILPOM's
displaced surface. Atom applies the decal at the resolved SILPOM shading
position. Decals affect color and optionally shading normals; they do not change the height field, silhouette,
neural model, physics, or the stair materials. Projection depth controls how
thick a volume receives a dab. Use a shallow depth where nearby walls should
remain separate. Back-facing surfaces are rejected by angle attenuation.

Depth snapshots are asynchronous, at most about seven per second, and requested
only while the visible painter is enabled. Static views can use the latest
snapshot for every pen event without waiting on the GPU. Closing the pane or
disabling painting stops readbacks. The current picker requires the main
viewport's depth attachment at its native resolution.

## Gaomon tablet

The viewport accepts Qt tablet press/move/release events with continuous pressure.
**Pressure controls size** and **Pressure controls opacity** are independent.
Minimum pressure size keeps light pen contacts usable. Mouse painting uses full
pressure. The live **Pen** line shows the device name, pressure and tilt delivered
by the driver; tilt is diagnostic and does not change the brush shape. Handled
tablet gestures suppress duplicate mouse stamps.

Keep the Gaomon driver running. This Windows installation has its driver and
Wintab library installed. If strokes work but the input line stays on Mouse,
check the driver's application profile for Editor.exe and its pen/Wintab versus
Windows Ink setting. The tool consumes tablet events supplied by Qt; it does
not install a driver or alter system tablet settings. Test a light-to-firm stroke
and watch the pressure readout before tuning the brush response.

## Rendering and limits

The latest 100 placements remain editable and serializable alongside the fixed
Composite. Each paint entity supports 64 layers and one render batch. A retained
invisible material anchor per brush keeps its packed texture indices valid; these
anchors do not occupy GPU light-list slots. There is no entity per dab, and no
readback or tablet processing in a game build. The renderer's texture-array and
scene culling limits still apply to distinct materials and paint entities.

For diagnostics, `r_silpomPaintCompositeReference true` evaluates the same stream
without spatial skipping or the alpha early-out. Restore it to `false` after
comparison; the reference can be much slower on large paintings.

## Verification

- `SilPOM.DecalPaint` tests event-rate-independent spacing, corners, gaps,
  pressure, normalized normals and bounded emission; the five existing shader
  CPU suites run alongside it.
- `Tests/decal_painter_editor_smoke.py` runs a dedicated Editor session and checks
  the actual dock, rendered-depth picking, stamps, continuous strokes, undo/redo,
  layer locking/visibility, synthetic tablet pressure, point editing and prefab
  serialization. It exits without saving VueLiveLink and writes results under
  `TGProject/user/DecalPainterSmoke`.
- Synthetic pen events verify the software input path. Physical tablet pressure
  must be checked with the user's pen.
- `Tests/decal_painter_composite_smoke.py` checks 5000 dabs, exact tail retention,
  native undo/redo across the irreversible frontier, serialization, a gesture
  longer than 100 dabs and rendered equivalence to the complete ordered stream.
- `Tests/decal_painter_erase_height_smoke.py` checks independent range controls,
  erase undo/redo and synthetic pen pressure, all three height bands, filtered
  erase inside Composite, prefab serialization, lower-layer preservation, later
  repainting, normal-paint erasing and optimized/reference rendered comparisons.
- `Tests/decal_painter_event_loop_smoke.py` returns control to the normal Qt
  event loop before testing focus, painting and ordinary viewport selection. It
  verifies that event timers continue and the rendered frame remains visible,
  then closes without saving the level.

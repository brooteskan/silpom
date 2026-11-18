# Engine requirement

SilPOM builds against O3DE 2.7.0 (`061180bf`) with the patch bundle in
[brooteskan/o3de-patches](https://github.com/brooteskan/o3de-patches). The
exact revision SilPOM is tested with is pinned in `O3DEPatches.json`.

SilPOM needs only the bundle's `engine` component. The engine changes it calls
are the decal blend modes and paint compositing (`DecalComposite.azsli`), the
explicit-gradient shading hooks (`ATOM_EXPLICIT_SHADOW_GRADIENTS`,
`ATOM_EXPLICIT_LIGHTING_LOOKUPS`, `ATOM_CUSTOM_GOBOTEXTURE_GRADIENTS`,
`ATOM_CUSTOM_SPECULAR_AA`) and the RPI shader-input accessors. A stock O3DE
engine does not build or run SilPOM.

```powershell
git clone https://github.com/brooteskan/o3de-patches $patches
git -C $patches checkout <commit from O3DEPatches.json>
python "$patches/apply.py" --engine $engineSource --skip terrain-compositor
```

See the bundle's README for building a project against the patched engine.

Tests that read the patch text (`Tests/test_native_paint_shader.py`) and editor
harnesses that record the bundle in their evidence find the checkout through
`SILPOM_O3DE_PATCHES` (or the CMake cache variable `SILPOM_O3DE_PATCHES_DIR`),
then a sibling `o3de-patches` folder next to this gem or its parent folder. A
checkout whose bundle or engine-patch hash differs from the pin is rejected.
Without a checkout those tests report themselves skipped.

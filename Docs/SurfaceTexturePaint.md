# Persistent surface-texture paint

Surface-texture paint defines its result directly in saved receiver textures.
The old projected-dab document and its optional UV prefix cache remain a
different representation. The new textures are source paint data, not a
disposable acceleration of a replay stream.

## The normal-cache error

The old bake evaluates a sequence of normalized normal blends at a particular
material normal `N0`. It stores `bias = F(N0) - T*N0`, where `T` is the product
of the normal transmittances. Rendering reconstructs
`normalize(T*N_live + bias)` after sampling and filtering those coefficients.

Repeated normalized blends are not an affine function of their incoming normal.
The coefficients can reproduce the particular baked value at a texel, but do
not define the same result for a different incoming material normal. Filtering
world-space coefficients across texels, then combining them with a separately
evaluated live normal, introduces another mismatch. Normalization, interpolation
and evaluation at a varying base normal do not commute. Removing undo does not
correct this mathematics.

A second, independent error affected both texture paths: each mip is a separate
texture descriptor, so per-pixel mip selection must use
`NonUniformResourceIndex`. Without it, compiled DXIL marked the descriptor
handles uniform and neighboring GPU lanes could sample the wrong mip image.
The raw editor capture measured sparse errors of 15.0039 degrees maximum in
normal and 0.32398 maximum in color before this correction. Both paths now mark
that selection nonuniform directly at the bindless texture lookup. The engine's
nullable material-texture helper has a conditional null fallback which discards an
upstream nonuniform marker, so validated paint-table indices bypass that helper.
The binding correction also applies to legacy mode
2; its fitted coefficient normal model remains an approximation and is not
accepted by the new texture oracle.

`Tests/SurfacePaintMathTests.cpp` includes a concrete two-stamp counterexample:
an affine fit made at one base normal differs by more than one degree when
applied to a different base normal. The persistent representation below does
not make that fit or promise equivalence to historical projected normal stamps.

## Saved ownership and texels

A chart belongs to a receiver entity, canonical face, paint layer and face side.
Its UV rectangle and resolution identify the texture domain. Each texel stores
two independent four-component values:

| Texture | RGB/XYZ | Alpha |
|---|---|---|
| Color | Premultiplied linear paint color `C` | Color coverage `A_color` |
| Normal detail | Premultiplied unit detail vector `M` in the receiver frame | Normal coverage `A_normal` |

The receiver frame is attached to the canonical face. Its tangent follows U;
its bitangent is computed from the side's base normal, tangent and original UV
handedness. Flipping sides flips the frame normal and bitangent together.
The frame is independent of the current material normal
and SiLPOM's height-derived shading normal. These live normals still come from
the existing surface renderer. Object movement rotates the frame with the
receiver; the camera never chooses paint ownership.

Source texels are saved losslessly as float32 data in the chart payload. The
payload has explicit dimensions, validation and a checksum. Derived mip levels
can be rebuilt from those source texels. Reloading painted work does not require
the old brush material, individual dabs or the receiver material normal used
while painting. The source heightmap, relief geometry and silhouette are not
paint data and are not modified.

Source chart resolution is a power of two from 32 through 1,024, with 256 used
for new charts. A receiver supports at most 128 face/layer/side charts and a
256 MiB budget for the active layout's raw GPU mip texels, whichever limit is
reached first. Color and normal detail each use RGBA32F: together they require
32 bytes per texel. A 256-square chart with its complete mip chain therefore
uses about 2.67 MiB of raw GPU texels. This budget excludes descriptor tables,
material/allocation overhead, CPU source data and temporary pending or retired
GPU snapshots used for atomic replacement; peak memory while editing is higher.

## Applying a brush

A brush evaluates the receiving surface and its texture footprint while
painting. It updates affected texels immediately. Once a successful edit has
been encoded in the saved chart, the brush's individual projected dabs are not
needed to render or reload the result.

For a decoded unit normal detail `d` and effective normal opacity `a`:

```text
M_new = (1-a)*M_old + a*d
A_new = (1-a)*A_old + a
```

`d` lies in the outward hemisphere of the receiver frame. A flat normal map is
`(0,0,1)` and stays neutral over curved or displaced surfaces. Brush orientation
rotates its XY detail into the receiver frame; it must not turn the brush's
absolute projector-forward direction into saved normal detail. Color coverage
and normal coverage are independent.

Color is bounded linear RGB in `[0,1]`. Alpha, Multiply, bounded Add, Screen and
Overlay operate within the selected, isolated paint layer. They do not depend
on the live material or a separately evaluated lower layer while painting.
With source color `s`, opacity `a`, stored premultiplied color `C`, coverage `A`
and blend function `B`, the isolated layer update is:

```text
C_new = (1-a)*C + a*((1-A)*s + A*B(C/A,s))
A_new = (1-a)*A + a
```

An empty layer receives `a*s` for every blend mode. Alpha uses `B(d,s)=s`;
Multiply uses `d*s`; Add uses `min(1,d+s)`; Screen uses `1-(1-d)*(1-s)`;
Overlay chooses its branch using the existing layer color. Layer rendering
then composites the stored result over the material or lower paint layers.

## One defined normal composition

Rendering linearly samples the saved normal tuple and forms the covered detail:

```text
D = normalize(M + (0,0,1)*(1-A_normal))
```

In the receiver frame, the renderer rotates `D` by the shortest arc from `+Z`
to the live material normal. It normalizes once after that rotation. This is
one definition of the painted normal result. It is shared by the portable CPU
reference and `ReceiverUv/SurfaceNormal.azsli`.

Untouched texels and flat detail return the live normal exactly. The antipodal
case has a defined 180-degree rotation. Opposing detail vectors that cancel
produce neutral detail. Neither authoring nor rendering stores a bias fitted
against a baked base normal.

## Filtering and erase

Filter all four normal components together. Never normalize `M` after a brush
update or when constructing mip levels: its magnitude records coverage and
detail cancellation. The shader adds uncovered neutral area only after
filtering. For example, one quarter opaque `+X` detail and three quarters clear
area gives `M=(0.25,0,0)`, `A=0.25` and final detail proportional to
`(0.25,0,0.75)`. Normalizing the stored mip vector first would incorrectly
amplify the painted edge. Color uses ordinary premultiplied filtering.

Erasing with strength `e` scales premultiplied values and their coverages by
`1-e`. A full erase clears the selected layer's texels and reveals the material
and separately retained lower paint layers. It cannot recover overwritten paint
in the same flattened layer: blue followed by opaque red and then erase reveals
the underlying material. Keeping blue in a lower layer preserves it.

Undo is independent of this representation. An undo implementation can retain
changed texture regions or snapshots; permanent projected-dab history is not
required by the saved texture result.

## Validation scope

The portable tests cover clear and flat identity, varied live normals, exact
CPU/shared-shader rule agreement, receiver-frame rotation, mip reduction,
premultiplied edge filtering, isolated color blends, erasing and separate layers.
The same production shader helper is compiled unchanged through a C++ primitive
adapter in that suite. AddressSanitizer also checks the portable suite.

`Tests/SurfacePaintNormalGpuTests.cpp` dispatches the actual shared normal helper
on DX12, including hardware bilinear/trilinear sampling of an RGBA32F texture
with the authoritative raw-component mip chain. On an RX 7900 XTX, both SM 6.2
and SM 6.7 passed 6,144 cases: 2,048 direct tuples, 2,048 filtered mip-chain
samples and 2,048 divergent separate-mip-descriptor samples over varied live
normals. Normal error against the CPU texture reference was
**0.02326 degrees RMS / 0.25889 degrees maximum**, below 1 degree RMS / 5 degree
maximum. All 897 clear/flat cases preserved the live normal bit-for-bit. The
direct-math maximum component error was 0.00001544; hardware interpolation's
maximum tuple component error was 0.002352. The separate old-affine
counterexample measured 10.2073 degrees, demonstrating the fitted model's issue.

The separate-descriptor cases vary mip selection within GPU waves. With the
required nonuniform annotation they measured 0.02209 degrees RMS / 0.19087
degrees maximum. A negative control removing only that annotation failed on
both shader models: 12.73333 degrees RMS / 54.25819 degrees maximum and 131
failed clear/flat identity cases. This isolates descriptor selection from the
normal composition and interpolation mathematics.

`Tests/test_surface_texture_shader_bindings.py --dxc <path-to-dxc>` compiles the
actual production texture traversal for SM 6.2 and SM 6.7 and checks the paint
texture handles' nonuniform flag in DXIL. It also compiles a temporary copy
with those annotations removed and requires that same guard to reject it.
It also requires rejection of a control routing the marked index through the
engine's exact conditional null-fallback helper. Optional `--engine-hlsl FILE
MIN_HANDLES` and `--engine-variant FILE MIN_HANDLES` checks compile full generated
engine HLSL or extract the real cached pixel bytecode and verify its bindless
array, rather than relying solely on the test harness. The production source is not modified
by either negative control.

These standalone checks establish the defined composition and filtering for
synthetic inputs. The bounded editor image test below compares rendering with
the authoritative texels and filtering rule, without fitting it to the old
projected normal sequence. Other resolutions, chart boundaries, camera views
and material variants still need measured image checks before broader acceptance.

`Tests/surface_texture_editor_smoke.py` bounds the real-editor image fixture to
the base SiLPOM material path. It temporarily disables the experimental 4A/4B
and 5A/5B material modes through the editor component API and restores all
affected settings before exit, without saving the level. Its raw diagnostic
captures the actual material color and normal before and after painting, source
UV and UV gradients for each real MSAA sample. The independent image oracle
samples the saved texels at those observed addresses and gradients, bounds the
documented material UV round trip, and checks both the raw result and the real
Forward pass's packed normal. Lit diffuse output is used for unchanged-output
checks, rather than as a substitute for material albedo. Image results from
this fixture do not establish learned/shared/proxy material image quality.

The image oracle permits interpolation tolerances only where normal detail can
be sampled. It separately requires bit-exact live normals when every possible
bilinear tap at both selected mip levels has zero XY normal detail. Merely
getting zero from an ideal weighted sum does not qualify: hardware weight
quantization can disturb cancellation or zero-weight-neighbor cases. Receiving
faces without a visible saved chart must preserve native color and packed
normals exactly.

## Editor result, 2026-11-16

The [recorded proof](Benchmarks/SurfaceTexturePaint-20261116.json) passes on an
RX 7900 XTX using DX12, a fixed 1280x720 editor view and 2x MSAA. Five saved
charts render with **zero retained dabs**. The fixture contains 1,617,652
receiving samples; numerical quality evaluates 665,448 chart samples, including
265,816 with normal detail, after the documented derivative-quad exclusions.

Worst results across the two real MSAA samples are:

| Comparison with saved-texel reference | RMS | Maximum |
|---|---:|---:|
| Raw painted normal, degrees | 0.005836 | 0.090655 |
| Actual Forward packed normal, degrees | 0.073180 | 0.227282 |
| Raw linear albedo, absolute channel error | 0.000107 | 0.001525 |

The direct bindless lookup removes the pre-fix raw-normal maximum of 15.0039
degrees and albedo maximum of 0.32398. Actual cached Usage and Forward pixel
bytecode contains the required 4 and 8 nonuniform texture handles; their hashes
remain stable after asset processing finishes. Both shader models also reject
negative controls that omit the marker or route it through the nullable material
helper.

Flat detail, source-neutral raw and packed normals, uncharted faces, cleared
material, restored painted output, hit records and depth all pass their exact
preservation gates. Erase reduces both coverages, real Editor undo/redo restores
the texels exactly, and serialized source texels survive clear/reload. The level
and heightmaps are unchanged; all temporarily changed material settings and
existing paint documents are restored. All 24 image-oracle regression tests and
the four portable/GPU CTests pass. Native Static, runtime, Editor and Builder
modules build, and final asset processing reports zero failed assets and errors.

This is image-quality acceptance for the stated base-material fixture. It does
not measure frame-time gains or learned/shared/proxy material image quality.
Existing projection documents remain legacy data; their fitted normal cache is
still an approximation, even though its mip descriptor lookup is corrected too.

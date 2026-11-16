# Receiver-owned UV paint proof

**Measured prototype; opt-in.** The crash/resource-binding checks pass, but
normal accuracy and minified performance do not meet acceptance. See the
[benchmark and limits](ReceiverUvPaintBenchmark.md). Startup mode remains native
replay.

This proof starts with an existing static SiLPOM receiver. A newly authored dab
stores `PaintSurfacePoint { receiver entity, canonical face ID, UV, front }`.
The painter resolves a contact against a currently active receiver before adding
the dab. Consolidation copies this address unchanged. Position and normal fields
in the old document format are compatibility/display data; an anchored dab's
render position and frame are derived from its receiver and UV. Missing receiver
or canonical-face references produce no projection. They never become free
projectors that can paint a future object.

The first bake accepts one paint entity, one canonical receiving face, one
visible layer, an alpha-only fixed prefix, and a live tail without erases.
Unanchored legacy documents are ineligible. Conversion of existing paint must
resolve against its existing receiver explicitly; a material/height texture
shared by two objects is not an ownership key.

The fixed prefix is capped at 32,768 dabs and 65,536 traversal nodes, matching
the shader's bounded traversal. Oversized consolidated histories retain native
replay and report the eligibility failure; they cannot publish an empty atlas.

## Surface and texture association

The persistent source key is `(receiver entity ID, canonical face ID)`. A mesh
generation resolves this to its unique runtime hit-face ID and batch-local face
index. Runtime IDs are not saved. Replacing the material on the existing mesh
handle preserves hit IDs while comparing the three render modes.

The receiver owns a UV rectangle and three RGBA16F 2D images per mip: paint color
and coverage, and front/back normal coefficients. A small bindless table stores
the resolved face ID, rectangle, prefix count and these image handles. The
forward shader checks the hit's receiver face and samples with its face UV.
Camera position is absent from this mapping. UV derivatives select ordinary
filtering levels; they do not select surface ownership or reject the cache.

Each mip is rendered from source dabs over this face's 2D UV domain, including
two-texel gutters. The bake reconstructs the existing surface at each UV using
the same height and affine displacement direction functions. It evaluates the
receiver material and ordered stamp color/normal blends there. Reconstructed
positions are temporary evaluation inputs, not addresses of a 3D cache.

The fullscreen bake supplies explicit scene-material type/instance indices in
its pass data. The standard zero-argument material getter requires a mesh's
`ObjectSrg` and cannot be used here. The compiled entry-point dependencies are
checked by `Tests/receiver_uv_shader_bindings.py` before rendering tests.
The pass also binds the receiver's actual object SRG and a defined instance
offset because the retained pipeline layout declares those slots. A skipped
draw invalidates the benchmark even when a diagnostic reports an atlas lookup.

Color stores premultiplied paint and coverage. Each normal image stores
`bias = composedNormal - transmittance * materialNormal` and normal
transmittance. Rendering applies these to the current surface material normal.
Filtering and partially transparent normal chains are approximations to measure
against replay; two-sided samples use separate coefficients.

SiLPOM's heightmap, bounds, hit solver and learned visibility inputs are read
only. Stamp height is not introduced. Existing height masks select which surface
samples receive paint.

## Fixed prefix and live tail

The receiver shader either replays the fixed prefix or reads its atlas, then
claims that paint entity's prefix token. The native composite shader skips that
prefix only for the shader invocation that claimed it and renders the ordered
live tail. Native anchored dabs carry a receiver-instance token so the brush
footprint can cover adjacent triangles of that receiver without leaking onto
another object. The canonical face remains the stamp's center anchor and the
single-face cache address; it does not clip native brush coverage. Color-only
unbound streams retain front-to-back opacity early-out with receiver rejection.
The native reference traversal also honors a prefix already applied by the UV
material, so it never applies that prefix twice.

Mode 0 uses the original material and full native stamp replay. Mode 1 uses the
UV material with fixed-prefix replay. Mode 2 uses the UV material and atlas,
replaying while the atlas is being built or outside its UV rectangle. This is
a surface-domain decision. Images remain associated with the same face as the
camera moves.

Fixed source edits, stamp texture reloads, receiver transforms and generation
changes invalidate derived resources. Live-tail edits retain the fixed atlas.
The painter polls a transform/generation revision for every receiver referenced
anywhere in the document, including after legacy entries and across layers.
Disabling the cache pauses pending bake passes. Clearing the document or losing
the receiver prevents stale cached paint from rendering.

## Verification

`Tests/receiver_uv_benchmark.py` runs an isolated Editor module, queries an
existing tower face, creates 2048 consolidated dabs and 100 live dabs addressed
to that face, and compares modes 0/1/2 in forward/reverse order. Captures and
diagnostic passes are disabled for GPU timing.

`r_silpomMsaaInspect 1` also installs a UINT4 usage pass cleared to zero:
`{ use (0=no attempt, 1=prefix replay, 2=atlas), hitFace, replacedCount, owner }`.
The inspect shader exports each real MSAA sample into a single-sample atlas at
`x*4+sample`. Hit records, depth, usage, diffuse and encoded normals are captured
for both samples. Cache activity is never inferred from scene RGB.

The analysis checks exact hit/depth bits, explicit atlas use, unchanged other
faces, live-tail edits, invalid-face and zero-prefix cases, and close/ordinary/
minified quality. The source level and height image hashes are checked without
saving the level. Benchmark and image-quality results must be reported before
expanding to multiple charts, layers or erase operations.

The review fixes add portable tests for explicit R8/R16/R32 decoding, malformed
payloads, scaled relief and slope normals, every referenced receiver's revision,
and oversized prefix fallback. The production CPU geometry is included directly
by the surface test and is also checked under AddressSanitizer. Each published
mesh retains its source configuration, base scale and rigid transform, so a
pending replacement cannot make old face records index a different surface.

`Tests/test_native_paint_shader.py` executes the shipped native traversal through
a C++ adapter with deterministic texture samples. It checks adjacent-triangle
coverage, receiver isolation, opaque early-out and single prefix handoff in the
reference path. `Tests/test_receiver_uv_analysis.py` supplies synthetic negative
controls for replay normals, other-face normals and residual orphan/clear paint.
These checks do not establish atlas quality or GPU performance; the opt-in
acceptance limits above still apply.

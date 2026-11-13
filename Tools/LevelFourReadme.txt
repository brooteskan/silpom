Level 4A: experimental learned wall material
==========================================

This implements the material-decoding part of Level 4. It is an opt-in
comparison candidate, not an established performance improvement. No GPU
benchmark was run for this change. Shared material/visibility features (4B)
are a separate default-off experiment; see LevelFourFiveBReadme.txt.

Use
---
Select the tower in VueLiveLink, open its SilPOM Mesh component, and enable
"Level 4A wall material (experimental)". It requires "Local shadow method" =
"Neural only" (enum 3) and the existing supported Level 3 relief depth 0.5.
The checkbox defaults to off; no saved level or material is changed.

The trained source is Assets/SilPOMLearned/wall.l4mat in the TG project, built
into assets/silpomlearned/wall.silpommaterial by Asset Processor. The trained
binary belongs to the project's asset repository, not the SilPOM Git repo.
Missing, stale, malformed or incompatible assets produce an explicit status.

Scope and removed work
----------------------
Only opaque fieldstone-wall material batches with the original canonical
basecolor.png, normal.png and roughness.png assets and wall neural model 4
are selected. Other materials are independently assigned the ordinary path
when batches are prepared. Extra textured channels on a matching material
are rejected. A disabled emissive map is allowed and omitted from the copy.

The learned forward variant consumes the same winning hit and constructs
Surface directly. It does not call EvaluateSurface_StandardPBR, bind/copy
the three replaced maps into the decoded material, or evaluate them first.
Base-color factors/blending, map enable flags, normal strength/flips/frame,
scalar metallic/specular/roughness/opacity/emissive and clearcoat settings
are retained. The current canonical material uses UV channel zero and no
additional textured channels. This is not a general StandardPBR compressor.

The existing hit reconstruction, depth, height-derived geometric normal,
runtime decals, external shadows and dynamic-light plumbing are preserved.
The separate learned-material forward shader also defines the neural-only
Level 3 visibility contract: no local-shadow marcher or conventional surface
evaluator is included in that compiled forward variant. Intersection work
in the existing depth stage is unchanged by Level 4A.

Representation and filtering
----------------------------
Two RGBA8_UNORM feature images contain eight jointly trained channels at
1664 x 1536 with all 11 mip levels. Payload: 27,262,928 feature bytes plus
1,240 coefficient bytes. These are payload sizes, not measured GPU memory.

The decoder is 12 -> 16 ReLU -> 6, with 310 float32 coefficients. Inputs are
eight filtered latent values and four footprint values (log major/minor
extent and double-angle orientation). Outputs are linear-sRGB base color,
encoded tangent-normal G/R, and roughness. PC normal swizzling and material
flips follow the existing StandardPBR convention.

The shader computes a stable principal footprint from UV derivatives,
limits anisotropy to eight, takes one to eight directional trilinear samples
from each feature image, and runs the decoder once after filtering. LOD is
adjusted for the quarter-resolution feature atlas. This complete filtering
cost and decoder arithmetic must be counted in the later benchmark.

Training uses the same latent interpolation, UNORM8 quantization, mip
selection and directional filtering, with repeat seams, multiple footprints
and grazing orientations represented from the outset. All feature mips are
jointly trainable; they are not ordinary automatic downscales of latent mip 0.

Reproduce
---------
Tools/material_codec.py requires Python, NumPy, Pillow and PyTorch. Runtime
and the asset builder do not require PyTorch. The candidate used CPU PyTorch
2.14.1+cpu, 1,500 steps, batch 512 and the fixed seeds in the tool:

  python Tools/material_codec.py <wall-source-folder> <output-folder> --steps 1500 --batch 512

<wall-source-folder> is the TG project's
Assets/SilPOMPlanarAB/IrishFieldstoneTowerWelded/wall. Copy the resulting
wall.l4mat into Assets/SilPOMLearned and run Asset Processor. Large training
outputs, held-out queries, Python packages and screenshots belong in ignored
scratch/user directories. LevelFourQuality.json records the compact result.

The builder verifies a SHA256 of the three source PNGs. Retrain if any of
them changes. Generation IDs include all bundle bytes, so runtime images
are not reused across different weights or feature atlases.

Quality limits
--------------
The teacher is a filtered source-PNG mip pyramid: linearized base color,
renormalized normal mips and scalar roughness. It is not the engine's exact
BC-compressed texture output, texture importer, or final rendered lighting.
The held-out source-teacher test has 32,768 queries. Channel mean absolute
errors are approximately 0.0074/0.0066/0.0056 color, 0.0080/0.0078 encoded
normal, and 0.0059 roughness. Normal angular error p95 is 4.41 degrees.
These are approximation metrics, not a visual or performance acceptance gate.
The first editor comparison visibly softens fine wall detail in close-up.
Keep the ordinary material available for that quality comparison.

Validation of this candidate: 26/26 standalone tests passed; the trained
coefficients passed 4,096 CPU/GPU comparisons on each shader model (maximum
decoder output difference 2.98e-7); all editor assets processed successfully.
The editor test passed at three cameras, with identical winning UV/depth/status
before and after switching material evaluation, functioning spiral brightness,
height-normal toggles, and a real decal stamp plus undo. Original controls
were restored and the saved VueLiveLink prefab SHA256 stayed unchanged.

Correctness tests
-----------------
The standalone CTest suite includes header/payload validation, CPU decoder
behavior, D3D12 CPU/GPU decoder and footprint agreement on SM6.2 and SM6.7,
and independent filtering-oracle comparisons. Configure SILPOM_TORCH_PATH to
the directory containing torch to enable the optional Python filtering test.
MaterialCodecGpuTests also accepts a raw .l4mat bundle after the shader path
to check its actual trained coefficients, without collecting GPU timings.

Tests/Run-LevelFourSmoke.ps1 uses a dedicated Editor with fixed cameras,
render readbacks and a temporary decal canvas. It switches the representation,
checks the winning hit/depth, toggles geometric normals and spiral lighting,
and exercises decal painting/undo. It restores original component settings,
camera and console variables, exits without saving, and never invokes the
benchmark capture service. Close the Editor/Asset Processor before running it.

After the outline is implemented, compare forward and frame GPU cost,
actual resident material memory, and appearance during motion at grazing
angles with otherwise identical geometry, lights and Level 3 visibility.
Keep this default-off candidate as a compression option if it saves memory
but adds frame time; Level 5 does not depend on using the Level 4 decoder.

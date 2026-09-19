# SilPOM surface contract, version 1

The patch is a finite sheet in local XY, displaced along +Z. It is not a solid
box. Backing, end caps and corners are authored geometry. The proxy only bounds
possible hits; a miss must reveal the background.

## Physical coordinates

`z = heightScaleMetres * (H(u,v) - referenceHeight)`.

The component resolves positive uniform entity scale into width and height,
then submits a rigid metre-space transform. Height is always in world metres.
Nonuniform scaling is an incompatible component service. Runtime animated
transforms are not supported by the static motion-vector path.

The image must contain normalized linear scalar height data and have exactly
one mip. R32_FLOAT, R16_UNORM and R8_UNORM are accepted. The fixture tool requests
the engine's LUT_R32F preset with mip generation disabled. Color and normal maps
remain independently filtered. Geometry cannot change because a streaming mip,
shadow derivative, or screen edge changes.

Texture interpolation is bilinear with centers at `(i + 0.5) / size`. Address
mode 0 repeats; mode 1 clamps individual taps. UV tiling can be negative. Color,
normal and AO use the resolved hit UV. An authored normal map describes the full
relief slope in the planar tangent frame; it is not added a second time to the
height-derived geometric slope.

## Intersection

`Assets/Shaders/SilPOM/Heightfield.azsli` is shared by raster material evaluation,
the Atom procedural intersection adapter, compute tests, native DXR tests, and
CPU shader-conformance tests. The independent double oracle enumerates all cells
and derives its polynomials separately.

The kernel clips a ray to a conservative box, visits texel cells in ray order,
and solves the bilinear surface polynomial within each interval. The root solve
uses normalized interval coordinates and returns the nearest root. Ray direction
is not normalized after a coordinate transform: `t` retains the caller's units.
The caller supplies its allowed ray interval.

Statuses are Hit, Miss, Exhausted and Invalid. A raster exhausted search emits a
magenta diagnostic surface; it is not a valid quality fallback. A configuration
showing this diagnostic must not be accepted. Increase the cell budget, reduce
authored tiling/height complexity, or add a conservative traversal accelerator.

## Raster adapter

The component uses Atom's existing MeshFeatureProcessor public API. A two-triangle
quad covers the conservative projection of the 3D bounds. Ambiguous near/eye-plane
projection falls back to viewport coverage. No scene-depth reconstruction or
off-screen framebuffer data is required. Bounds are set explicitly for culling.

Custom-Z material variants write actual hits in camera depth, forward color and
shadow maps. Shadow rays start at conventional near Z; camera rays start at
reversed near Z. The NDC interpolant is sample-qualified for MSAA. Misses discard
in all visibility passes. Static camera motion reconstructs position from hit
depth; the stock mesh motion draw is disabled because its vertices are proxies.
Reflection-probe capture and triangle-based RT registration are disabled.

## Future procedural ray tracing

`SurfaceDescriptor.h` defines a 48-byte surface and 64-byte procedural record.
The descriptor has no raster dependency. A future owner registers
`Intersection.shader` through `RegisterProceduralGeometryType`, supplies a
bindless descriptor buffer, and registers each patch's conservative AABB via
`AddProceduralGeometry`. The local instance index selects its descriptor.

Descriptors and textures must remain resident until all in-flight frames finish.
Publish new resources, bounds and descriptor generations together. A transform
change can update TLAS state; a physical footprint/height-range change also
requires a bounds update. Do not reuse a descriptor slot until its prior GPU
generation is retired. Disable proxy triangle registration before adding the
procedural representation to avoid duplicate or flat occlusion.

Atom's compact intersection attributes carry position, UV, geometric normal and
tangent. Material identity remains in scene/instance buffers. The internal hit
record is not the DXR attribute payload. Additional closest-hit material behavior
and RT effect integration require validation before enabling a production effect.

## Acceleration seam

Min/max pyramids, cone maps, tighter raster coverage and tiled BLAS bounds can
replace candidate traversal without changing this surface contract. Acceleration
data must derive from the exact decoded image, be versioned, cover bilinear tap
footprints and seams, and remain conservative. Averaged mipmaps are not bounds.
Any accelerator must pass the existing nearest-hit/UV/normal conformance tests.
Hardware RT accelerates patch selection; it does not make heightfield sampling
free. Benchmark AS updates, shading and shadow costs separately.

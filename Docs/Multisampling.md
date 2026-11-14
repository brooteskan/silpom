# Sample-frequency hit reuse

The DX12 MainPipeline has an opt-in four-sample path. The default remains the
approved single-sample renderer. Height reconstruction, trained weights, and
material-footprint filtering are shared between them.

In VueLiveLink, select **Irish Fieldstone Tower (Open Shaft)** and open its
**SilPOM Mesh → Anti-aliasing** dropdown:

- **Current default (unchanged)** preserves the approved single-sample default.
- **SMAA** enables both Atom's SMAA passes and its SMAA feature processor at 1×.
- **TAA** enables temporal AA at 1×.
- **4x MSAA (editor comparison)** selects the experimental four-sample renderer.

SMAA and TAA carry into game mode. The selection changes scene AA without regenerating
the tower or changing its estimator, lighting, recovery or quality settings.
Returning to Current default, or disabling the component, restores the settings
active before the comparison. Default components are passive. If several meshes
make explicit selections, the most recently edited selection wins; releasing it
returns to the remaining selection. MSAA's sample count affects the application,
including preview scenes, so a brief rendering pause protects sample transitions.
Existing levels deserialize to Current default; no tower prefab needs rewriting.

Use four samples for editor comparisons. **After using MSAA, select Current
default, save and restart the Editor before Ctrl-G.** The MSAA-to-game handoff
currently exposes a native DX12 `SkinnedMeshOutputStream0` UAV/read-state barrier
mismatch, including when the sample count has already returned to one. Merely
selecting a different AA option does not establish that gameplay is safe in
that session. The selector does not claim to fix this engine limitation.

Four samples add substantial GPU cost and remain an experimental coverage
comparison. They retain the current raw/repair console choice; selecting AA does
not alter that choice or add diagnostic passes. Unsupported devices retain 1×.
`Tests/AntiAliasingSelectorEditorSmoke.py` exercises the actual dropdown property,
rapid transitions, disable/re-enable and exact approved 1× captures. Gameplay
after an MSAA comparison is explicitly outside this editor visual suite.

The visual suite does not collect timings by default. Set
`TG_SILPOM_MSAA_TIMING=1` to add the existing 120-frame GPU captures. New visual
reports verify SMAA through its active pipeline mode and enabled feature
processor; older reports can still use their recorded pass timing evidence.
The established 1× reference includes classify/fill and the backup shell, while
raw 4× omits them and repaired 4× only repairs fully covered, same-owner pixels.
Consequently these images compare recovery policy as well as AA. They do not
establish that MSAA alone improves silhouettes. Do not fill genuine partial
sample coverage with the backup shell to match the repaired 1× image.

TGProject's terrain-compositor integration also needs
[the clipmap pass lifecycle fix](Integration/TerrainClipmapMsaa.patch), included
here for reproducible VueLiveLink builds. A sample-count change replaces the
pipeline's root without replacing the pipeline object. Terrain must recreate its
cached passes for that new root and avoid queuing detached old passes for removal.
Otherwise the terrain cache can disappear and Ctrl-G can assert after an MSAA
comparison. Apply the patch from the terrain-compositor repository and rebuild
`Terrain.Static`, `Terrain.Editor` and `Terrain` alongside the SilPOM modules.

The equivalent manual four-sample controls remain available:

```text
r_antiAliasing MSAA
r_silpomMsaa4 1
```

Disabling `r_silpomMsaa4` restores one sample. The native rendering device is
checked for four-sample render/load support on the actual attachment formats
before enabling the path. Pipeline roots pause during the sample-count
transition, including root recreation, so old draw packets cannot be submitted
to new attachments. The transition owns its tick handler until completion and
drops removed pipelines; disabling the last mesh cannot leave surviving roots
paused. The three-tick delay remains a scheduling guard rather than a general
shader/readiness guarantee.

Each covered sample has its own unresolved `uint4` hit record. Intersection uses
sample-interpolated camera coordinates. The forward wrapper supplies
`SV_SampleIndex` to hit loads and reconstructs the winning sample's world
position from that record's depth. Shell and proxy UV/depth are also
sample-interpolated; their established center-depth expression is retained in
the NoMSAA variant. No integer hit attachment is resolved.

## Coverage and inspection

`r_silpomMsaaRawCoverage 1` is the initial four-sample mode. It disables the
heuristic classify/fill passes. Setting it to zero enables conservative
sample-aware repair: a pixel must already contain four valid wall samples with
the same owner. Partial coverage, mixed owners, and ordinary foreground samples
cannot be expanded by this repair. The raster backup plane is always disabled
at four samples. Single-sample repair is unchanged.

`r_silpomMsaaInspect 1` adds diagnostic passes; leave it off when measuring GPU
cost. Raw/final hit and scene-depth exports preserve sample identity in a
four-times-wide atlas. Diffuse/specular exports capture shaded samples before
opaque color resolve. These exports avoid Atom's generic floating-point MSAA
attachment readback, which cannot preserve integer hit records.

## Downstream lighting

The SilPOM wrapper retains Atom's material, decal, direct-lighting and output
code. Decals therefore receive the reconstructed sample position rather than
the proxy position. Albedo, normals, material properties and depth remain
multisampled through the opaque indirect-lighting composite. That composite
loads the same sample from each attachment before evaluating lighting.

Atom averages the shaded diffuse attachment. Its existing specular resolve
averages in log space with neighborhood luminance clamping; it is not an
arithmetic HDR average. Screen-space consumers receive Atom's existing
sample-zero resolved depth. They do not consume or replace the unresolved hit
owners. Screen-space reflections, AO and temporal reconstruction retain their
existing single-depth approximation.

The four-sample hit image is initialized by a small pixel-frequency shader
clear. Native `ClearRenderTargetView` of this multisampled integer target hung
on the tested DX12 device at larger viewport sizes. The single-sample image
retains its existing native clear. This workaround changes initialization only;
the hit solver and record layout are unchanged.

## Validation workflow

Run `Tests/MsaaAssetReadiness.py <project>` before starting the isolated Editor.
It checks generated supervariant metadata, actual preprocessed include times,
both DX12 variants, build errors, and the preserved forward PassSrg layout.

`Tests/MsaaEditorSmoke.py` first captures the lit approved 1x reference and
stops immediately on a reference-image or binding failure. It then compares
none, SMAA, TAA, a two-times-width/height supersampled diagnostic, and four-sample
MSAA at identical cameras, with geometric LOD selection disabled. Stationary
and slow-camera-motion captures use the same height and learned assets.

Four-sample exports test partial coverage, different owners, ordinary geometry,
sample-specific UV/depth, repair protection, and the diffuse color resolve. A
separate controlled skylight fixture exercises indirect illumination. Steady
GPU timestamps exclude inspection, screenshots and readbacks, and run after
warm-up with VSync and the frame limiter disabled. The scripts restore their
temporary settings and never save the source level.

`Tests/MsaaVisualTests.py <capture-root>` verifies the approved twenty image/hit
captures exactly and checks the per-sample invariants. Its downsampled PNG
comparison is a display-space diagnostic, not a linear-HDR ground truth.

Atom accepts `MSAA`, `SMAA` and `TAA` names. A numeric `r_antiAliasing 0` does
not disable an already active TAA pass. The capture helper explicitly selects
`MSAA` for the no-post-AA and four-sample comparisons. This project's SMAA
configuration also starts disabled; the editor-only benchmark hook enables its
processor during the SMAA comparison and restores its initial state afterward.
The source configuration and the user's saved AA settings are not changed.

## Validated result, 2026-11-13

The [compact validation report](../Tests/Reference/MsaaValidation.json) records
the settings provenance, sample checks and timing statistics. All twenty
approved static/motion images and normalized hit records match exactly. All
35 standalone CTest groups pass. Twelve actual generated pixel shaders,
including near/shell/proxy depth and shared/angular forward shaders, compile
under SM6.2. The running O3DE comparison and DX12 validation use SM6.7; this is
not an end-to-end SM6.2 engine run.

At 1280x720 on the RX 7900 XTX, median GPU graphics spans over 120 steady frames:

| Setup | 1x | 4x sample-frequency |
|---|---:|---:|
| User's saved estimator 5, ambient accessibility and Level 5A/5B | 3.78 ms | 12.84 ms |
| CurrentRouting, ambient accessibility and Level 5A/5B disabled | 4.25 ms | 14.48 ms |

The shader clear costs 0.018 ms. In the saved setup, hit generation costs
1.70/7.89 ms and forward shading costs 0.72/3.18 ms at 1x/4x. Graphics spans
include other scene passes and queue gaps; they are not GPU busy time or an
FPS conversion. The observed unthrottled Editor capture rates for the saved
setup were approximately 142/67 FPS. Capture/compilation stalls and a
one-second viewport overlay are not steady GPU measurements.

The sample tests cover 2,093 partially covered pixels, 48,208 mixed-owner
pixels, ordinary geometry occlusion, distinct sample UVs, exact sample-depth
agreement, protected repair, and more than 7,000 selected proxy samples in
each proxy-depth mode. Diffuse resolve agrees with independently shaded
samples within half-float precision. A lit skylight fixture adds measurable
indirect illumination before resolve. The saved estimator/ambient/proxy
combination also passes the sample checks and DX12 validation at 1280x720.

Raw 4x was more expensive and had greater display-space edge error against the
downsampled diagnostic than SMAA/TAA in these three views. It remains an
experimental comparison option. The approved 1x reference, solver, learned
weights, height reconstruction and saved tower settings are preserved. Decals
retain the common forward implementation and sample world position; this
suite does not include a separate decal-projector visual fixture.
Raw 4x also exposes residual intersection holes hidden by the established 1x
repair path. It is not a replacement for the approved repaired 1x appearance.

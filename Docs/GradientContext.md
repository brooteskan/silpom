# Winning-sample gradient context

The forward resolve uses a local continuation of the selected sample's surface.
It does not differentiate UVs or positions of neighboring visibility winners.
Ownership rejection can therefore occur before material evaluation, including
at boundaries between faces in the same material batch and different roots of
the same heightfield.

Two sample-matched RGBA32F attachments accompany the unchanged packed hit:

| Attachment | Components |
| --- | --- |
| `HitUvGradients` | source `du/dx, dv/dx, du/dy, dv/dy` |
| `HitDepthGradients` | recorded `dz/dx, dz/dy`, representation, validity |

The unit is change per raster pixel at the current sample position. Neither
sample count nor sample-index differences scale the footprint. The pass SRG
provides the actual raster viewport extent independently of attachment size.
World-position slopes differentiate inverse projection at the stored center
depth, so repairs and quantized packed depth share the same position anchor.

Relief uses the exact sampler's one-sided bilinear cell and affine, unnormalized
displacement direction. Base fallback has explicit provenance. Shell and backup
use the actual displaced triangle. Smooth LOD differentiates both endpoints,
the complete moving smoothstep interval, and its weight in UV and position.
Learned proxy depth differentiates its complete owner-local mapping numerically
with a symmetric 1/16-pixel stencil, retaining its established center evaluation
and raster footprint. Footprint variation at virtual locations is analytic and
anchored to that center. Proxy appearance retains its separate authored-plane
query and conditioned footprint.

Copied repair inherits donor slopes. Same-chart arithmetic and depth-weighted
bridges preserve their existing center arithmetic, vary weight along the donor
separation, and extend donor fields perpendicular to it. Classification reads
the original hit and sidecars, without cascading. Fill and backup publish the
paired hit and sidecars under the same existing coverage/depth conditions.
Clear and MSAA resource lifetimes match the hit attachment.

Representation occupies the low eight metadata bits: relief 1, base 2, shell 3,
backup 4, morph 5, authored proxy 6, learned proxy depth 7, diagnostic 11. Repair
provenance occupies the next bits: copy 256, arithmetic 512, depth-weighted 768.
Validity is 1 for a conditioned mapping and 2 for the coarse policy. Singular,
nonfinite or excessively large mappings use at least one full source chart and
one texture period, constant recorded-depth slopes, and maximum bounded specular
AA variance. They never manufacture a small determinant or zero UV footprint.

StandardPBR and learned maps, receiver UV paint, and projected decals receive
explicit gradients. When specular AA is enabled, four normal-only queries use
this owner's continuation at +/-0.25 raster pixel around the current sample.
Standard and learned queries retain the center UV footprint; learned queries
prepare its eigensystem, taps, LOD, tap offsets and context once. Proxy queries
cache owner-plane axes and metrics, then recompute the restored plane position,
UV, footprint conditioning and view for each offset. They bypass generic
virtual geometry and keep their inputs local, without publishing center
appearance or lighting state. Feature values and hidden activations remain
specific to each query.

The center's complete surface runs once, last, before receiver paint and
decals. AA-disabled builds and `o_applySpecularAA=false` skip all four queries.
Normal-map disablement skips learned feature decoding, while retaining the
stencil for geometric normal variation. Proxy normal enable semantics stay
unchanged. A separate flag accumulates any coarse or nonfinite probe; the AA
callback combines source, probe and center appearance validity and preserves
Atom's bounded coarse response. This deliberately changes AA near a
conditioning boundary where a valid center previously hid a finite coarse
probe. The stored source context is unchanged. Directional
shadow bias differentiates the actual flat-caster receiver advance and complete
cascade projection. BRDF/LTC lookup and preconvolved diffuse-environment fixed
LOD policies are opt-in engine hooks; ordinary engine materials retain their
existing behavior. See the frozen engine integration README for resource
contracts and guarded patch hashes.

Receiver chart baking has no screen-visibility winners. It supplies an explicit
source-UV atlas texel step for material filtering, while its continuous chart
normal acquisition retains the existing atlas derivatives. The UV usage
diagnostic binds the sample sidecars and inherits the same viewport constant as
the forward resolve. Its separate two-output inspector preserves the existing
usage atlas format.

This changes the shading contract. Analytic differentials and owner-local
normal variation need not reproduce hardware finite differences or the learned
models' former footprint features. Hit records, coverage and depth are the
invariance checks; legacy shaded colors are a measured comparison. Corrected
full and disjoint-rectangle resolves must match each other exactly.

The final two sidecars alone cost 56.25 MiB at 1280×720 and 2× MSAA. Repair
staging, bandwidth and virtual material evaluations add further cost. This is a
correctness-first implementation, not an assertion of a performance gain.

The pre-refinement gradient-context validation used 1280×720 at 2× MSAA,
height-derived normals enabled,
and the four frozen benchmark views plus a wider authored-proxy probe. All
seven attachments matched bit-exactly between full-screen, disjoint rectangles
and repeated full-screen controls. The four benchmark views preserved all
7,372,800 hit/depth samples against the frozen legacy renderer; shading was
compared on 5,541,526 owned samples. Including the wider probe, all 5,557,447
owned contexts were finite, with 60 defined coarse cases and cleared unowned
sidecars. The wider probe exercised 2,563 authored-plane proxy samples.

A separate paint run on identical renderer sources exercised a 64² receiver
atlas with two fixed and two live dabs. All seven attachments and the usage
atlas matched exactly across scheduling modes. It used the atlas on 5,183
samples, with visible fixed and live contributions. The temporary binding
republished the mesh generation: its raw owner IDs shifted uniformly by the
native declared canonical-face mapping. The proof records that shift and
requires exact UV/depth/flags/status and canonical ownership, while ordinary
benchmark comparisons remain raw-bit exact. The entity, binding and settings
were restored without saving the level.

The scene did not select morph, learned proxy-depth or diagnostic samples, or
copied-donor repairs. Their producer/policy shaders compile; the differential
and conditioning tests cover their math. The paint material has a constant
normal image; this run does not prove varying paint-normal texture filtering
or ordinary non-composite decal rendering. Those consumers have compiled
explicit inputs.

The standalone gradient suites pass 11,875 CPU checks and both base and repair
hardware programs at SM6.2 and SM6.7. Current-source forward variants contain
zero reachable implicit derivative or texture/LOD instructions. Native code
builds with strict MSVC warnings; the processed asset and MSAA readiness checks
have no stale products or failed jobs. Capture methodology is in
[GradientContextValidation.md](GradientContextValidation.md). Detailed readbacks
and reports are under `TGProject/user/silpom-gradients-20261116/quality-final`
and `paint-pilot`. The subsequent normal-query refinement is validated
separately against these captures. Its shaders request normal inputs and
decoder head columns only; they omit unrelated color, roughness, opacity,
visibility and lighting acquisition. StandardPBR uses its shared normal-input
helper, preserving UV transforms, overrides, precision and backface handling.

Timing was stopped at the user's request before this refinement. The partial
`benchmark` directory retains 18 of 24 captures from the preceding build and
an interruption record. It is not a completed benchmark of the refined code.

The normal-query refinement passed its separate five-view 2× MSAA comparison:
all 35 full-resolve attachment comparisons against the full-surface-query
baseline were bit-exact, including 5,557,447 owned shaded samples. All 70
current full/rectangle/control attachment comparisons also passed. Stored
hits, depth and gradient sidecars were unchanged. The 260 controlled renderer,
fixture, harness and binary files stayed unchanged during capture, and the
temporary settings were restored. Results are in
`normal-only-quality/refinement-report.json` beside the earlier captures.

Scratch-only shader instrumentation verifies zero virtual normal queries with
AA off and exactly four with AA on in all four forward families; it is absent
from production. Builds with `ENABLE_SPECULARAA=0` also compile. The normal
decoder heads passed 49,152 bit-exact column comparisons over random and frozen
models; the extended hardware tests also pass at SM6.2 and SM6.7. These checks
establish work gating and output preservation, not a measured speedup.

The hit path requires draws to use their raster pass viewport. `MeshController`
acquires its private generation models through `MeshFeatureProcessor::AcquireMesh`
and consumes the resulting `MeshDrawPacket`s; it does not author packet viewport
overrides. Atom's mesh packet builder leaves its viewport array empty, so every
current relief, shell, backup, proxy and forward producer inherits the pass
viewport. The disjoint resolve clones those packets and changes geometry and
scissors only. `HitRasterPass` records that actual viewport after frame setup
and before the pass SRG is compiled. Externally authored draw packets with a
per-item viewport override require a matching per-draw gradient extent and are
outside this pass-viewport-only contract.

Repair publication also applies `SilPomGradientConditionRepair`: a valid
synthetic context remains valid only while each source-UV derivative component
and each recorded-depth derivative component has absolute value at most one
per raster pixel. Nonfinite or larger components select the unit diagonal UV
coarse marker with zero depth slopes and unchanged representation/provenance.
Existing coarse contexts and empty invalid contexts remain unchanged. This
bounds depth-weighted quotient amplification near zero recorded depth even
when both donor mappings are individually conditioned. The forward owner
promotes the coarse marker to its actual full source chart and at least one
complete source-UV texture period before filtering and specular AA; the hit's
stored center UV and depth do not change.

After validation, the refined build completed the requested [four-view 2× MSAA benchmark](NormalOnlyResolveRyzen5700G.md): 24 captures and 2,880 frames, with inspection disabled and all 260 controlled hashes unchanged. The preceding interrupted timing data is excluded.

The subsequent [normal-query setup refinement](NormalQuerySetupRefinement.md) reuses invariant learned/proxy setup and propagates any coarse probe independently of center lighting state. Its five-view 2x MSAA readback matches the preceding corrected renderer and all scheduling controls exactly. No new timing is claimed for that refinement.

[Face/view caching](FaceViewCache.md) adds generation-owned coefficients and frame-local implicit depth coverage/bounds. Its validation keeps depth, ownership and shading exact, with an explicit two-ULP UV/gradient roundoff bound; it is not a bitwise hit-record equivalence claim. No new cache timing is reported.

# Why 57 upper-glancing proxies receive depth 1.0

**The current bound includes offscreen portions of long wall faces.** Forty-one
of the 57 overlapping proxies have an expanded AABB reaching behind the camera
plane; another sixteen have their nearest AABB point between the eye and the
0.2 m near plane. The vertex shader therefore intentionally returns or clamps
to reverse-Z depth 1.0. This is conservative for the full box, but often much
looser than necessary for the part visible to the camera.

**Tighter conservative bounds are geometrically possible without changing the
accepted hit/recovery domain.** Intersecting the same expanded AABB with the
camera frustum tightens **53 of the 57**. A separate tolerance-padded displaced
triangle enclosure tightens **55**. These are offline calculations only: no
shader, renderer, scene, default, sampling option or baseline capture changed.

## Captured inputs and exact cause

This analysis uses the existing 2x upper-glancing frame on SILPOM main
`2f818d87cd29663fab360cc7ccde06e117c257cf`, event **473**, from the preceding
[paired RenderDoc investigation](RenderDocMSAA2Ryzen5700G.md). It replays through
command-line `qrenderdoc --python`, reads constants/buffers and performs CPU
analysis. It requests no counters, shader replacements or Windows setting
changes. Developer Mode is not needed.

Captured vertex/material state:

- Camera position: `(1.0041462, -1.0697279, 18.1824780)` m.
- Perspective reverse Z, near **0.2 m**, far **1024 m**; identity tower model transform.
- Raw material scale **0.0500000007 m**, reference **0.0500000007**: displacement extrema approximately **-2.5 to +47.5 mm**.
- Model 4, validation iterations 4, retry cap 2, tolerance multiplier 5.
- **Height-derived normals/sampling on**, confirmed both by fixture metadata and the captured material flag; gather and R16 remain on.
- 571 face descriptors, 172 raster-overlapping proxies in the frozen footprint model.

The decoded material identity, face-buffer index and offsets were checked
against the captured shader layout. Reconstructing all **571** constant proxy
depths from captured matrices/descriptors differs from GPU post-VS output by at
most **8.5848e-7**, below the existing 1e-5 depth bias. Eligibility comparisons
use the exact captured GPU bounds, rather than those reconstructed approximations.

[SilPomFaceBounds](../Assets/Shaders/SilPOM/BlendedFace.azsli) computes a local
AABB around six displaced triangle extrema. Then
[SilPomNearestFaceDepth](../Assets/Materials/Pipelines/MainPipeline/SilPOMHitDepth.azsli)
expands its half-extents by 2e-5 and evaluates:

```text
minW = center.w - dot(abs(localToClip.rowW.xyz), extent)
maxZ = center.z + dot(abs(localToClip.rowZ.xyz), extent)
bound = minW > 1e-6 ? clamp(maxZ/minW + 1e-5, 0, 1) : 1
```

The 57 cases split exactly as follows:

| Cause | Proxies | Captured-input diagnosis |
| --- | ---: | --- |
| W fallback | **41** | All have negative minimum clip-W, not merely a tiny positive denominator. |
| Positive-W saturation | **16** | Minimum W ranges **0.009663–0.194321 m**, entirely before the 0.2 m near plane. |

The issue is not primarily an inaccurate `maxZ/minW` division. In this captured
projection, clip-Z and clip-W have nearly proportional spatial coefficients
(residual below 5.81e-12), so their extrema are correlated. Computing the maximum
Z/W over the eight box corners, with the required W-crossing guard, removes
**zero** of the 57 depth-1 bounds and leaves the eligibility counts unchanged.
Removing the W guard or arbitrarily capping below 1
would not be a conservative fix.

Also, **51 of the 57 six-extrema volumes cross/touch the near plane somewhere**.
Simply replacing the AABB with the six-corner hull still includes offscreen
near-plane crossings. Conditioning the enclosure on the visible frustum is the
larger distinction.

## What tighter bounds preserve

For an unchanged expanded box B and camera frustum F, every relevant covered
hit is in `B intersect F` in the geometric model. A face-constant bound can be:

```text
min(1, max over p in B intersect F of clipZ(p)/clipW(p) + existing depth bias)
```

This retains the **same AABB**, including the padded domain used by ray tests
and fallback handling. It changes the domain over which the nearest visible
depth is bounded, not the neural solver or recovery rule. Camera side planes
exclude points that cannot contribute to a covered viewport sample.

The CPU tool enumerates feasible polytope vertices, rounds candidate depths
upward and retains the 1e-5 bias. It then checks nonnegative combinations of
the enclosure inequalities, with a bounded residual over the parent AABB and
a rounding allowance, proving `Z - bound*W <= 0` over the **whole candidate
enclosure**. Uncertified results retain the original bound. For nonempty
frustum-clipped AABB cases, there were **zero certificate fallbacks**; the
largest certified upper limit, including allowance, remains below -2.23e-6.

An independent check uses exact integer ownership/depth records from both
MSAA samples immediately after the dominant draw. **Zero of 1,804,749 recorded
winning samples exceeds any reported candidate bound**. Their encoded depths
also match the depth attachment bit-for-bit. This empirical check alone would
not prove safety for overwritten or unseen hits; the enclosure calculation
addresses the wider domain.

### Which depth-1 cases remain

| Offline enclosure | Original 57 tightened | Kept at 1.0 | Explanation |
| --- | ---: | ---: | --- |
| Eight-corner ratio, no visibility conditioning | 0 | 57 | Same full-box near/camera crossings. |
| Same expanded AABB intersected with frustum | **53** | 4 | Faces 299/300/301 genuinely reach the visible near plane; face 504 retains its old bound for an empty intersection. |
| AABB plus expanded proxy screen rectangle | **53** | 4 | Only a small further eligibility change. |
| Tolerance-padded triangle hull, visible portion | **55** | 2 | Face 301 reaches the visible near plane; face 504 again retains its old bound. |

Face 504's empty intersection has a separate nonnegative-constraint
contradiction certificate (gap 0.03193 in normalized inequalities). It was
**not culled** or counted as a tightened depth bound in these comparisons.
The remaining visible near-plane intersections legitimately require 1.0 for
their respective enclosures; a universal bound below 1.0 is not justified.

The padded hull is a more conditional candidate than the unchanged-box method.
For the captured model-4 path, displacement direction is affine in triangle
barycentrics and normalized height lies in [0,1]. The enclosure includes the
validator's `-2e-6` barycentric allowance, configured Newton residual tolerance,
extra 2e-5 padding, accepted base-triangle fills and convex shell morphs.
It must not be substituted as an unpadded ideal-surface hull or generalized to
other output/fallback contracts without checking them. The unchanged-AABB
frustum result does not depend on that additional surface-contract analysis.

## Potential eligibility, not measured speedup

The following uses the **unchanged proxy footprints** and captured depth before
and after event 473. It assumes the standard Direct3D 2x sample positions, as
in the earlier coverage model. It does not simulate in-draw depth evolution,
hardware Hi-Z granularity, helper lanes or a changed renderer.

| Bound model | Eligible against pre-draw depth | Eligible against completed depth |
| --- | ---: | ---: |
| Current GPU bounds | 22,680,953 | 14,440,274 |
| Eight-corner ratio | 22,680,953 | 14,440,274 |
| Same AABB intersected with frustum | **22,335,328** | **8,410,221** |
| Plus proxy screen rectangle | 22,334,907 | 8,408,601 |
| Tolerance-padded visible hull | 22,255,067 | 7,850,132 |

The frustum-only box model reduces completed-depth eligibility by **41.76%**,
but pre-draw eligibility by only **1.52%**. Actual behavior depends on evolving
depth and hardware rejection. Neither percentage is a measured PS-invocation
reduction or a prediction that the **182.09 ms** baseline will improve by that
amount. No new GPU timing, replacement shader or visual comparison was run.
The existing 16.88-million invocation measurement remains unchanged.

## Concrete example: face 308

This wall face's AABB spans local Z **11.52–20.60 m**, over **9.08 m**, while
its relief range is only 5 cm. The full box has minimum W **-0.719589 m**, so
the shader returns 1.0. Its nearest visible box point is at W approximately
**0.300 m**, giving a conservative candidate depth **0.6665693**. Its relevant
support constraints are two box sides and the camera's bottom side plane.
Across the 53 tightened depth-1 cases, the bottom plane participates in 44
certificates (some cases use multiple camera side planes).

The [local geometry figure](../../../user/silpom-depth-bounds-20261114/analysis-final/face-308-frustum-bound.png)
shows the long offscreen volume and the much smaller visible intersection.
It is an offline geometry illustration, not a modified rendering.

## Reproduce and preserve the baseline

Work is isolated on `codex/depth-bounds-ryzen-5700g`, starting from profile
commit `7228815`. Main remains `2f818d8`; the completed benchmark branch remains
`18c33bb`. All **24** raw normal-clock captures and the previous scene/config/
pin/report fingerprints verify unchanged. No Editor/build/asset processing,
shader source or runtime changes, sampling toggle, Windows setting change or
new counter run was required.

The final read-only extraction is in
`TGProject/user/silpom-depth-bounds-20261114/captured-inputs/`; final CPU results,
per-face certificates and the figure are in `analysis-final/`. Compact results,
all 57 cases, provenance and raw-file hashes are recorded in
[Ryzen5700GDepthBounds20261114.json](../Tests/Reference/Ryzen5700GDepthBounds20261114.json).

The saved-frame extraction can be repeated without launching Editor:

```powershell
$project = 'D:/diversion/TG/TGProject'
$repo = "$project/Gems/silpom"
$old = "$project/user/silpom-renderdoc-msaa2-20261114"
$raw = "$project/user/silpom-depth-bounds-20261114"
$rd = "$project/user/silpom-profile-ryzen-5700g-20261114/tools/RenderDoc-1.47/RenderDoc_1.47_64"
$out = "$project/user/depth-bounds-repeat-$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory -Path "$out/inputs" | Out-Null
$config = Get-Content "$raw/captured-inputs/config.json" -Raw | ConvertFrom-Json
$config.output = "$out/inputs"
$env:SILPOM_COVERAGE_CONFIG = "$out/config.json"
$config | ConvertTo-Json -Depth 6 | Set-Content $env:SILPOM_COVERAGE_CONFIG
Start-Process "$rd/qrenderdoc.exe" -ArgumentList @('--python', "$repo/Tests/TowerBenchmarkDepthBoundProbe.py") -WorkingDirectory $rd -WindowStyle Hidden -Wait
$state = Get-Content "$out/inputs/depth-bound-probe.json" -Raw | ConvertFrom-Json
if (-not $state.passed) { throw $state.error }
$python = 'C:/Users/david/AppData/Local/Programs/Python/Python312/python.exe'
& $python "$repo/Tests/analyze_tower_depth_bounds.py" --project $project --inputs "$out/inputs" --capture-data $old --output "$out/analysis"
if ($LASTEXITCODE -ne 0) { throw 'Offline verification failed' }
# NumPy is required. Optional --plot uses Matplotlib; output must be fresh.
```

The probe reads the captured material's actual bindless slot and face data;
the CPU tool checks material identity, normals-on state, reproduction accuracy,
depth/record equality and geometric certificates. The shader's GPU float32
implementation and behavior at other cameras/materials remain untested for
these candidate bounds. This establishes feasibility for analysis, not an
implemented or production-validated optimization.

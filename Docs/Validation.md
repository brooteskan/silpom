# Local v0 validation — 2026-09-19

Environment: Windows, O3DE 2.7.0 engine revision
`061180bf24f1666eb30315b35da292eb14f4659c`, Visual Studio 18 2026,
AMD Radeon RX 7900 XTX, DX12, TG DefaultLevel, 1280×720, MSAA 2×.

## Results

| Check | Result |
| --- | --- |
| Standalone CPU kernel | 24,832 checks passed, including 20,000 rays against an independent double oracle |
| D3D12 compute | 8,192 CPU/GPU comparisons passed, 1,956 hits |
| Native procedural DXR | 8,192 CPU/RT comparisons passed, 1,956 hits |
| Descriptor ABI | 48-byte surface and 64-byte procedural descriptor assertions passed |
| O3DE build | SilPOM.Static, SilPOM and SilPOM.Editor built in profile |
| Asset processing | Corrected final batch: 5 assets processed, 0 failed; all generated raster variants compiled in the preceding batch |
| Editor smoke | Passed; actual GPU model ready; five disable/enable cycles |
| Editor errors | None in the final run |

The smoke fixture uses TG's StoneWall color, normal, AO and height textures on
a 2 m × 2 m wall with 0.1 m signed displacement range and reference 0.5. The
height source SHA-256 is
`31675e93592a811662fe108623460b502191b2ad46f01941637344e952b5ee87`.

Ten captures were inspected: front, all four viewport boundaries, 80°/85°/88°
from the wall normal, a near-plane crossing, and an eye inside the conservative
volume looking along the wall. The wall remains visible across viewport clipping;
the displaced silhouette exposes background without filling the bounding box.
No magenta traversal failure was observed. At extreme close range the source
texture resolution and steep heightfield slopes are visible; this is not a
replacement for authored side faces or overhangs.

This is a render smoke test, not an image-oracle proof of every pixel. The shared
kernel's numerical tests provide the independent intersection check. Dedicated
shadow-map comparisons against tessellated ground truth, temporal motion-vector
readback/TAA sequences, many-patch stress tests, and Vulkan runtime validation
remain release-hardening work. No production Atom RT effect is enabled.

## GPU telemetry

Actual pass timestamps, in milliseconds, from matching views with the patch
enabled and disabled. These are single-frame whole-scene pass samples, including
terrain and other scene work. They are diagnostic measurements, not an isolated
benchmark or a POM speed comparison. Do not add nested pass timings together.

| View | Depth on / off | Forward on / off |
| --- | ---: | ---: |
| Front | 0.628 / 0.309 | 1.021 / 0.689 |
| 85° | 2.195 / 0.335 | 1.742 / 0.730 |
| Inside, looking along wall | 3.695 / 0.796 | 3.469 / 1.328 |

The unaccelerated exact traversal can be expensive at grazing angles. The v0
does not claim to outperform POM. Conservative min/max traversal or other
acceleration must preserve the current conformance results. Hardware RT patch
selection alone will not remove the per-heightfield traversal cost.

## Artifacts and reproduction

- Run the commands in the README for standalone tests and TG fixture preparation.
- Run `Tests/editor_smoke.py` in a separate Editor process. It creates an unsaved
  wall, restores viewport/background preferences and exits without saving the level.
- The local run wrote images, six raw GPU timestamp files and `result.json` to
  `D:/TG/TGProject/user/SilPOMValidation`.
- Build/test logs are in the ignored local `build` directory: `conformance.log`,
  `atom-build-4.log`, `asset-build.log`, and `asset-build-2.log`.
- The optional Atom intersection shader builds on DX12. Vulkan was disabled for
  that adapter after the engine's DXC crashed with an internal access violation.
  The ordinary raster material variants compiled for both DX12 and Vulkan.

The generated TG comparison material and copied single-mip height source live
under the project's `Assets/SilPOM`; original StoneWall textures and DefaultLevel
were not edited. `project.json` registers the local Gem checkout. Existing TG
working-tree changes were left in place.

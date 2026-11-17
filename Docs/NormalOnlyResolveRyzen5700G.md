# Refined disjoint resolve: Ryzen 5700G

On AMD Ryzen 7 5700G with Radeon Graphics / AMD Radeon(TM) Graphics (driver 31.0.21923.11000), the refined normal-only renderer was measured at 1280×720 with 2× MSAA and height-derived normals enabled.

| View | Resolve full / rectangles (ms) | Speedup | Reduction | Graphics span full / rectangles (ms) | Span speedup | Span reduction |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| exterior | 29.898 / 10.046 | 2.976× | 66.40% | 77.017 / 56.484 | 1.364× | 26.66% |
| lower-facing | 162.959 / 147.988 | 1.101× | 9.19% | 264.965 / 249.792 | 1.061× | 5.73% |
| lower-glancing | 168.661 / 148.492 | 1.136× | 11.96% | 346.819 / 325.815 | 1.064× | 6.06% |
| upper-glancing | 150.017 / 135.008 | 1.111× | 10.00% | 328.192 / 313.242 | 1.048× | 4.56% |

| View | Paired resolve speedup range | Paired graphics-span speedup range |
| --- | ---: | ---: |
| exterior | 2.968–2.990× | 1.362–1.365× |
| lower-facing | 1.101–1.102× | 1.060–1.061× |
| lower-glancing | 1.134–1.137× | 1.062–1.065× |
| upper-glancing | 1.111–1.111× | 1.046–1.050× |

Each mode/view has three interleaved 120-frame captures: 24 captures and 2,880 frames. Inspection and readback were disabled. The independent raw review verifies hashes, metadata, fresh timestamp ranges and zero positive inspection/readback timestamps. Graphics span includes queue gaps and is not GPU busy time or FPS.

The normal-only refinement preserves all seven compared attachments over 5,557,447 owned shaded samples in the five unpainted validation views. Current full/rectangle/repeated-full comparisons remain bit-exact. The four fixed views retain all 7,372,800 legacy raw hit/depth samples and compare 5,541,526 owned shaded samples. The earlier gradient correction's shading differences from the pre-gradient legacy renderer are a separate contract.

All 260 controlled source/fixture/binary hashes matched the validated build before measurement and stayed identical afterward. Hardware: 32 GiB RAM, DIMMs configured at 2133,2133 MHz, Balanced power policy. No fixed CPU/GPU clock is claimed.

Runtime representation categories not observed: morph, proxy-depth, diagnostic; repair categories not observed: copied-donor. Earlier paint proof is historical and excluded from this refined-build result. Final sidecars still cost 56.25 MiB, excluding staging and bandwidth.

The compact [measurement record](../Tests/Reference/Ryzen5700GNormalOnlyResolve20261116.json) retains timing, quality and integrity evidence. Raw capture data, the native analyzer report and the Editor log are in `TGProject/user/silpom-gradients-20261116/benchmark-normal-only`; independent review and before/after snapshots are in `scratch/normal-only-benchmark-20261116`.

These timings compare the same refined shading in both modes. The earlier pre-refinement partial benchmark was stopped and is excluded. No renderer source or saved scene was changed for this measurement.

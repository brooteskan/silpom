# SilPOMTowerTest: self-contained test project

This folder is an O3DE project, `SilPOMTowerTest`. It holds SilPOM's tower test
level and everything that level needs, so SilPOM's tower tests and assets build
without any consuming project. It enables the SilPOM gem from this checkout
(`external_subdirectories` points at `../../..`) and O3DE's stock gems.

| Path | Contents |
|---|---|
| `Levels/SilPOMTowerBenchmark/` | The tower test level (`SilPOMTowerBenchmark.prefab`) and its frozen tower configuration and views (`scene.json`). |
| `Assets/SilPOMPlanarAB/IrishFieldstoneTower*/` | The tower prefab, meshes (SilPOM surface, welded collision/GI proxy, roof lining), the `.silpom.json` surface, height images, materials, textures and import settings. Only what the level uses, directly or through the tower prefab. |
| `Assets/SilPOMLearned/` | Training-source lists for the learned bundles. The bundles themselves are built (see below) and git-ignored. |
| `Registry/silpom_tower_fixture.setreg` | `/SilPOM` registrations: trained models, learned wall material, learned bundles. |
| `Training/tower.json` | The level's training manifest. |
| `Gem/`, `CMakeLists.txt`, `cmake/`, `Platform/`, `Registry/`, ... | O3DE's DefaultProject template, instantiated for this project. |

Every asset keeps the project-relative path it had where the tower was
authored (`Assets/SilPOMPlanarAB/...`). O3DE derives source asset IDs from that
path, so the IDs stored in the level and the tower prefab resolve here.

## Using the project

1. Register it with your engine: `o3de register --project-path <this folder>`.
2. Configure and build it like any O3DE project (it has a project gem), e.g.
   `cmake -B build/windows -S . -G "Visual Studio 17"` then build `Editor`.
3. Run the Asset Processor or open the Editor with `--project-path <this folder>`.
4. Run the tower editor harnesses (`Tests/TowerBenchmarkEditorSmoke.py`,
   `TowerResolveRegions*.py`, `TowerRepairRegionsValidation.py`,
   `TowerGradientContextValidation.py`, `TowerFaceCacheBenchmark.py`, ...) in
   this project. They open `Levels/SilPOMTowerBenchmark` from the project root,
   or the level named by `SILPOM_TEST_LEVEL`. `Tests/Run-*.ps1` launch a
   dedicated Editor on this project by default.
5. `python Tools/prepare_wall_fixture.py` creates the `Assets/SilPOM` wall
   materials `Tests/editor_smoke.py` uses (git-ignored here).

The engine must have the [o3de-patches](https://github.com/brooteskan/o3de-patches)
bundle applied at the revision in `Integration/O3DEPatches.json`; see
`Integration/README.md`.

`Tools/prepare_tower_benchmark.py --project <other project>` still installs this
level into another project that already has the tower assets. Its `--generate`
mode rebuilds the level from `--source-level` (any level instancing the tower
prefab) and `--engine` (for the stock level template).

## Changes from the original project assets

`TowerTerrainSkirtBlend.material` originally used a project material type that
imported the terrain-compositor gem's `TerrainMeshBlend` type and sampled a
project ground texture in world space. Here it is a stock StandardPBR material
with the same color, F0 and roughness. That texture is a uniform grey (linear
0.51), so its color is reproduced exactly. The original faded the skirt normal
toward +Z over its bottom 0.15 m; this version does not. Images of the skirt
base are therefore not pixel-comparable with captures from the original level.

The level removes the tower prefab's diffuse probe grids and GI ray-tracing
proxy, as `scene.json` records under `removed_from_benchmark_only`. The tower
prefab still contains them, so the project enables DiffuseProbeGrid, and
includes the GI proxy material.

## Learned bundles are built, not committed

The level enables Level 4B shared features and Level 5B proxy depth, which use
`wall-shared.l4shared` and `wall-depth.l5depth`. Build them with SilPOM's tools,
passing this folder as the project root:

1. `Tools/material_codec.py` writes `wall.l4mat`.
2. `Tools/relief_appearance.py`, then `Tools/train_appearance_tail.py`, write
   `wall.l5app`.
3. `Tools/train_shared_depth.py` reads both and writes `wall-shared.l4shared`
   and `wall-depth.l5depth`.

Write the outputs to `Assets/SilPOMLearned/`. The SilPOM learned-material
builder checks each bundle against the files listed in its `.sources` sidecar.

## Training the level's models

`Training/tower.json` is this level's training manifest: the surface sidecar
and the four height profiles, with paths inside this fixture. SilPOM's
trainers read it in place of a project's authoring export.

The sidecar has tagged faces for profiles 1 (wall, 1130 faces), 3 (trim, 2028)
and 4 (reveal, 80). Profile 2 (stairs) has none, so its model cannot be trained
from this geometry and no face in this level uses it.

### Model 0

`Tools/train_model_zero.py` trains model 0 from the wall atlas at model 0's
calibration (0.02 m, reference 0.05, clamped):

    python Tools/train_model_zero.py Tests/Fixtures/TowerBenchmark/Training/tower.json <teacher library> <output folder>

The teacher library is `Tools/welded_tower_teacher.cpp` built as a shared
library (a Windows DLL, or on Linux with
`-D'__declspec(x)=__attribute__((visibility("default")))'`). The tool needs
NumPy and Pillow. It writes the uncached seed network, the tile-to-expert
layout, the RGBA16F seed cache with its import settings, the no-march hit
model, `model-zero.json` (all tensors and provenance) and `training.json`
(held-out accuracy). `--compare <SilPOM checkout>` also scores the weights
currently compiled into that checkout on the same held-out rays.

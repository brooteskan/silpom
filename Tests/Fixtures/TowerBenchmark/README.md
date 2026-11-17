# Self-contained tower test level

This fixture holds everything the tower test level needs, so SilPOM's tower
tests do not depend on any consuming project's assets.

| Path | Contents |
|---|---|
| `SilPOMTowerBenchmark.prefab` | Base level: camera, lights, stock O3DE ground plane and `LightingPresets/HighContrast` IBL. Stock components and engine assets only. |
| `scene.json` | Frozen tower component configuration and views. Its asset references resolve to files under `Assets/`. |
| `Assets/SilPOMPlanarAB/IrishFieldstoneTower*/` | The tower mesh, its `.silpom.json` surface, height images, materials and their textures, with import settings (`.assetinfo`). Only files the scene references, directly or through materials, are included. |
| `Assets/SilPOMLearned/*.sources` | Training-source lists for the learned bundles (see below). |
| `Registry/silpom_tower_fixture.setreg` | `/SilPOM` registrations for the tower's trained models, learned wall material and learned bundles. |

`Assets/` keeps each file's original relative path. O3DE derives source asset
IDs from that path, so the IDs in `scene.json` resolve unchanged.

## Changes from the original project assets

`TowerTerrainSkirtBlend.material` originally used a project material type that
imported the terrain-compositor gem's `TerrainMeshBlend` type and sampled a
project ground texture in world space. Here it is a stock StandardPBR material
with the same color, F0 and roughness. That texture is a uniform grey (linear
0.51), so its color is reproduced exactly. The original faded the skirt normal
toward +Z over its bottom 0.15 m; this version does not. Images of the skirt
base are therefore not pixel-comparable with captures from the project level.

The level omits the project's diffuse probe grids and GI ray-tracing proxy, as
`scene.json` records under `removed_from_benchmark_only`.

## Learned bundles are built, not committed

The level enables Level 4B shared features and Level 5B proxy depth, which use
`wall-shared.l4shared` and `wall-depth.l5depth`. Build them with SilPOM's tools,
passing this fixture folder as the project root:

1. `Tools/material_codec.py` writes `wall.l4mat`.
2. `Tools/relief_appearance.py`, then `Tools/train_appearance_tail.py`, write
   `wall.l5app`.
3. `Tools/train_shared_depth.py` reads both and writes `wall-shared.l4shared`
   and `wall-depth.l5depth`.

Each tool documents its arguments (`--help`). Write the outputs to
`Assets/SilPOMLearned/`; they are git-ignored. The SilPOM learned-material
builder checks each bundle against the files listed in its `.sources` sidecar.

## Using the fixture in a host project

A host project needs the SilPOM gem enabled and two additions:

1. A scan folder for this fixture's `Assets/` folder.
2. The settings in `Registry/silpom_tower_fixture.setreg`.

Register both only in a test project. They would conflict with a project that
already contains assets at the same relative paths.

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

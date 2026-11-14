# Portable tower benchmark

Open `Levels/SilPOMTowerBenchmark/SilPOMTowerBenchmark.prefab` in TGProject.
The scene contains the saved VueLiveLink tower at the origin, a small ground
plane, a directional sun and the engine's daylight HDRI sky/environment light.
Terrain, clouds, actors and unbaked diffuse-probe grids are omitted from this
copy. The tower's authored window area lights are retained for visible interior
comparisons. VueLiveLink and the tower's SilPOM configuration are
not edited. The shader's authored spiral lighting remains enabled as saved.
Two named, unshadowed comparison fills illuminate the interior camera positions
on all lighting channels so the column edges remain visible during inspection.

Choose **Camera | Exterior (Fly Camera)** for an overall view. The other named
cameras reproduce the lower facing and lower/upper glancing views used for
edge comparisons. They are editor-only. Ctrl-G starts the exterior fly camera;
use the normal fly-camera controls to move around or through the tower.

The saved AA choice remains **Current default**. The tower's SilPOM component
still exposes the existing estimator and AA selectors. Start comparisons at
1280×720 with the saved configuration, and record the actual GPU/driver on the
Ryzen 7 5700G machine. This scene does not impose machine-wide renderer options
or claim a measured speedup on that hardware.

## Getting the scene on the other PC

Pull Diversion **main**, including `Assets/SilPOMLearned` and the tower source
assets. Restore the Git gem revisions recorded in
`TGProject/scripts/GitDependencies.json` and build the SilPOM modules for that
PC's existing O3DE build. Allow Asset Processor to finish before opening the
scene. Use a shader target supported by the target GPU and driver; TG's build
scripts support the existing SM6.2 compatibility option where needed.

GitHub contains the frozen scene, configuration manifest and installer in the
SilPOM repository; the large tower textures, meshes and learned source assets
remain in Diversion. To install the frozen scene into an existing TG checkout:

```powershell
python Gems/silpom/Tools/prepare_tower_benchmark.py --project .
```

Run that command from `TGProject`. It refuses to overwrite a different scene
unless `--replace` is supplied. `--generate` explicitly rebuilds the fixture
from the local saved VueLiveLink tower; normal installation uses the frozen
snapshot and does not depend on the target PC's VueLiveLink edits.

## Repeatable checks

`Tests/TowerBenchmarkEditorSmoke.py` opens the scene, checks the saved controls,
captures all four camera views and checks Ctrl-G rendering. Run it with the
Editor `--runpython` argument and `TG_SILPOM_ESTIMATOR_OUTPUT` set to a fresh
output directory. It never saves either level. Optional
`TG_SILPOM_MSAA_TIMING=1` records 120-frame GPU timestamp captures at each view;
leave that disabled for visual validation. GPU data includes the device name,
resolution and sample count. Capture and timing runs should be separate when
making performance comparisons.

Run `Tests/TowerBenchmarkVisualTests.py <capture-directory>` to check that all
views are lit and contain tower hits. The checked-in validation report records
successful editor and gameplay captures; it contains no 5700G timing claim.

The configuration and camera coordinates are retained in the neighboring
`scene.json`. Treat that configuration as the starting reference for future
optimization work.

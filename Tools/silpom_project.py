"""Default project locations for SilPOM's offline tools.

Tools that read authored inputs (height maps, levels, training manifests) take
--project. It defaults to the bundled fixture project, Tests/Fixtures/TowerBenchmark,
so every tool runs from this repository alone; pass another O3DE project to use
its copies of the same assets.
"""
from pathlib import Path

GEM = Path(__file__).resolve().parents[1]
FIXTURE_PROJECT = GEM / "Tests/Fixtures/TowerBenchmark"
FIXTURE_MANIFEST = FIXTURE_PROJECT / "Training/tower.json"
TOWER_LEVEL = "Levels/SilPOMTowerBenchmark/SilPOMTowerBenchmark.prefab"


def add_project_argument(parser):
    parser.add_argument("--project", type=Path, default=FIXTURE_PROJECT,
                        help="O3DE project holding the authored assets (default: the bundled fixture project)")

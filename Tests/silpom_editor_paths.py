"""Locations used by SilPOM editor harnesses, independent of the host project.

GEM is this gem's root wherever it is checked out. PROJECT is the project the
Editor was launched with (azlmbr.paths.projectroot), or SILPOM_TEST_PROJECT /
the bundled fixture project outside the Editor. LEVEL is the level a harness
opens: SILPOM_TEST_LEVEL if set, otherwise the tower benchmark level of PROJECT.
Run harnesses against Tests/Fixtures/TowerBenchmark (project SilPOMTowerTest)
for a self-contained setup.
"""
import os
import sys
from pathlib import Path

GEM = Path(__file__).resolve().parents[1]
FIXTURE_PROJECT = GEM / 'Tests/Fixtures/TowerBenchmark'
TOWER_LEVEL = 'Levels/SilPOMTowerBenchmark/SilPOMTowerBenchmark.prefab'
TOWER_ENTITY = 'Irish Fieldstone Tower (Open Shaft)'

for _folder in (GEM / 'Tools', GEM / 'Editor/Scripts'):
    if str(_folder) not in sys.path:
        sys.path.insert(0, str(_folder))

try:
    import azlmbr.paths as _paths
    PROJECT = Path(_paths.projectroot)
    ENGINE = Path(_paths.engroot)
except ImportError:
    PROJECT = Path(os.environ.get('SILPOM_TEST_PROJECT', str(FIXTURE_PROJECT)))
    ENGINE = Path(os.environ['SILPOM_ENGINE']) if os.environ.get('SILPOM_ENGINE') else None

LEVEL = Path(os.environ['SILPOM_TEST_LEVEL']) if os.environ.get('SILPOM_TEST_LEVEL') else PROJECT / TOWER_LEVEL


def binary_dir():
    """Folder holding the running Editor's modules (SILPOM_BINARY_DIR overrides)."""
    if os.environ.get('SILPOM_BINARY_DIR'):
        return Path(os.environ['SILPOM_BINARY_DIR'])
    try:
        import azlmbr.paths as paths
        folder = getattr(paths, 'executableFolder', None)
        if folder:
            return Path(folder)
    except ImportError:
        pass
    return PROJECT / 'build/windows/bin/profile'

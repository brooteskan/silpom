"""Read-only decoder regressions; does not need Blender or O3DE."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'Tools'))
from validate_roundtrip import validate, negative_tests


class MetadataTests(unittest.TestCase):
    def setUp(self):
        fixture = ROOT / 'Tests/Fixtures/RoundTrip'
        self.sidecar = json.loads((fixture / 'roundtrip.silpom.json').read_text())
        self.imported = json.loads((fixture / 'roundtrip.imported.json').read_text())
        self.fbx = (fixture / 'roundtrip.fbx').read_bytes()

    def version_two(self):
        s = self.sidecar
        s['version'] = 2
        s['objects'] = [{'id': 'mesh', 'name': 'Authoring'}]
        s['logical_vertex_ids'] = {key: 'mesh:' + key for key in s['vertices']}
        names = sorted({f['material'] for f in s['faces']})
        s['materials'] = [{'id': name, 'name': name, 'export_name': name} for name in names]
        for f in s['faces']:
            f.update(stable_id='mesh:' + str(f['id']), object_id='mesh', material_id=f['material'], profile=1 if f['region'] else 0)
        self.resign()

    def resign(self):
        keys = ('vertices', 'logical_vertex_ids', 'faces', 'objects', 'materials')
        self.sidecar['semantic_sha256'] = hashlib.sha256(json.dumps({k: self.sidecar[k] for k in keys}, sort_keys=True).encode()).hexdigest()

    def test_legacy_baseline(self):
        self.assertTrue(validate(self.sidecar, self.imported, self.fbx)['passed'])
        self.assertEqual(negative_tests(self.sidecar, self.imported, self.fbx), 5)

    def test_generalized_baseline(self):
        self.version_two()
        result = validate(self.sidecar, self.imported, self.fbx)
        self.assertEqual((result['faces'], result['shared_edges']), (4, 3))
        self.assertEqual(negative_tests(self.sidecar, self.imported, self.fbx), 5)

    def test_namespace_corruption(self):
        self.version_two()
        self.sidecar['faces'][0]['object_id'] = 'other'
        self.resign()
        with self.assertRaisesRegex(ValueError, 'unknown face object'):
            validate(self.sidecar, self.imported, self.fbx)

    def test_duplicate_stable_id(self):
        self.version_two()
        self.sidecar['faces'][1]['stable_id'] = self.sidecar['faces'][0]['stable_id']
        self.resign()
        with self.assertRaisesRegex(ValueError, 'stable face identity'):
            validate(self.sidecar, self.imported, self.fbx)

    def test_nonfinite_direction(self):
        self.version_two()
        self.sidecar['faces'][0]['directions'][0][0] = float('nan')
        self.resign()
        with self.assertRaisesRegex(ValueError, 'displacement direction'):
            validate(self.sidecar, self.imported, self.fbx)

    def test_reversed_winding(self):
        self.version_two()
        altered = copy.deepcopy(self.imported)
        altered['meshes'][0]['faces'][0]['indices'].reverse()
        with self.assertRaisesRegex(ValueError, 'winding changed'):
            validate(self.sidecar, altered, self.fbx)


if __name__ == '__main__':
    unittest.main()

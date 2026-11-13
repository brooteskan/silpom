"""Solver diagnostics, independent roots and checkpoint quality gates."""
from pathlib import Path
import hashlib
import json
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Tools"))
import numpy as np
from PIL import Image
from level_one import Evaluator, load_weights, no_quality_regression, QUALITY_KEYS, outcomes
from level_one_reference import trace
from train_welded_tower import fit

BINARY = Path(sys.argv.pop(1))


def face(directions=None, uv_extent=1):
    return np.asarray([0,0,0, 1,0,0, 0,1,0,
                       0,0, uv_extent,0, 0,uv_extent,
                       *(directions if directions is not None else [0,0,1]*3),
                       0,0,1], dtype=np.float32)


class Profile:
    def __init__(self, height, packed=None):
        self.height = np.ascontiguousarray(height, np.float32)
        self.atlas = self.height.shape[::-1]
        self.scale, self.reference, self.address = .2, .5, 1
        self.packed = np.ascontiguousarray([face() if packed is None else packed])


def details(ray):
    return {"rays": np.ascontiguousarray([ray], np.float32), "ids": np.zeros(1, np.uint32)}


def weights(seed=0, classification=10):
    return [np.zeros((11,16),np.float32), np.zeros(16,np.float32),
            np.zeros((16,2),np.float32), np.asarray([seed,classification],np.float32)]


class LevelOne(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evaluator = Evaluator(BINARY)

    def test_iteration_budget_counts_actual_evaluations(self):
        profile = Profile(np.full((3,5), .75))
        ray = details([.2,.2,1,0,0,-1])
        hit, stat = self.evaluator.evaluate(profile, ray, 4, (1,0,1), weights())
        self.assertEqual(hit[0,0], 0)
        self.assertEqual(tuple(stat[0,[0,1,11]]), (1,1,1))
        hit, stat = self.evaluator.evaluate(profile, ray, 4, (2,0,1), weights())
        self.assertEqual(hit[0,0], 1)
        self.assertAlmostEqual(hit[0,1], .95, places=6)
        self.assertEqual(tuple(stat[0,[0,1,2,7]]), (2,1,1,0))
        self.assertLess(hit[0,4], 1e-6)

    def test_recovery_precedes_flat_fill_after_confidence_rejection(self):
        profile = Profile(np.full((3,5), .5))
        ray = details([.1,.2,.02, .9987492,0,-.05])
        hit, stat = self.evaluator.evaluate(profile, ray, 4, (2,1,5), weights(classification=-100))
        self.assertEqual(hit[0,0], 1)
        # A low-confidence prediction still gets a geometric grazing search.
        # This flat fixture has a real exact root, so no proxy fill is needed.
        self.assertEqual(tuple(stat[0,[0,1,5,6,7]]), (1,0,1,0,0))
        self.assertLess(hit[0,4], 1e-6)
        self.assertAlmostEqual(hit[0,1], .4, places=5)

    def test_shipping_and_exported_weight_order_match(self):
        header = Path(__file__).resolve().parents[1] / "Assets/Shaders/SilPOM/NeuralOpenShaftTowerWeights.azsli"
        profile = Profile(np.asarray([[.1,.7,.2,.8,.3]]*3))
        ray = details([.2,.2,1,.03,.01,-.9995])
        for model,name in enumerate(("Wall","Stairs","Trim","Reveal"),4):
            actual = self.evaluator.evaluate(profile,ray,model,(6,4,1))
            exported = self.evaluator.evaluate(profile,ray,model,(6,4,1),load_weights(header,"OpenShaft"+name))
            for a,b in zip(actual,exported):
                np.testing.assert_array_equal(a,b)

    def test_quality_gate_never_trades_wrong_depth_for_speed(self):
        baseline = dict.fromkeys(QUALITY_KEYS, 2)
        for key in QUALITY_KEYS:
            candidate = baseline.copy()
            candidate[key] = 3
            self.assertFalse(no_quality_regression(candidate,baseline))
        self.assertTrue(no_quality_regression(dict.fromkeys(QUALITY_KEYS,1),baseline))
        better = dict.fromkeys(QUALITY_KEYS,1)
        baseline["refined_depth_p95_mm"], better["refined_depth_p95_mm"] = .2, .23
        self.assertFalse(no_quality_regression(better,baseline))
        better["refined_depth_p95_mm"]=.205
        self.assertTrue(no_quality_regression(better,baseline))

    def test_checkpoints_are_independent_and_training_is_reproducible(self):
        random = np.random.default_rng(17)
        data = (random.normal(size=(100,11)).astype(np.float32), np.ones(100,bool),
                random.random(100).astype(np.float32), np.ones(100,np.float32),0)
        saved = []
        a = fit(data,12,steps=8,checkpoint_steps=(2,4),checkpoint=lambda step,w:saved.append((step,w)))
        b = fit(data,12,steps=8)
        for x,y in zip(a,b): np.testing.assert_array_equal(x,y)
        self.assertEqual([s for s,w in saved],[2,4,8])
        first = saved[0][1][0].copy()
        a[0][:] = 0
        np.testing.assert_array_equal(first,saved[0][1][0])

    def test_reference_variable_displacement_directions(self):
        f = face([0,0,1, .2,0,1, 0,.1,1])
        result = trace(np.full((3,5),.75),f,[.2,.2,1,0,0,-1],.2,.5,1)
        self.assertEqual(result["status"],"checked")
        self.assertEqual(len(result["roots"]),1)
        self.assertAlmostEqual(result["nearest_t"],.95,places=8)
        np.testing.assert_allclose(result["roots"][0]["uv"],[.2/1.01,.2/1.005],atol=1e-9)

    def test_reference_finds_all_crossings_and_nearest(self):
        image = np.asarray([[.1,.9]*8]*2)
        direction = np.asarray([1,0,-.1]);direction /= np.linalg.norm(direction)
        ray = np.r_[.05,.1,.05,direction]
        result = trace(image,face(),ray,.2,.5,1,t_max=.8)
        self.assertEqual(result["status"],"checked")
        self.assertGreaterEqual(len(result["roots"]),5)
        t = np.linspace(0,.8,50001)
        x = ray[0] + direction[0]*t
        field = .2*(np.interp(x,(np.arange(16)+.5)/16,image[0])-.5)
        residual = ray[2]+direction[2]*t-field
        ids = np.flatnonzero(residual[1:]*residual[:-1]<0)
        dense = t[ids]-residual[ids]*(t[ids+1]-t[ids])/(residual[ids+1]-residual[ids])
        np.testing.assert_allclose([r["t"] for r in result["roots"]],dense,atol=2e-7)

    def test_reference_singular_case_is_unresolved(self):
        result = trace(np.full((3,5),.5),face(),[.1,.1,0,1,0,0],.2,.5,1)
        self.assertEqual(result["status"],"unresolved")

    def test_captured_reveal_multiple_roots(self):
        fixture = json.loads((Path(__file__).parent / "Fixtures/level_one_reveal_rays.json").read_text())
        height_path = Path(__file__).resolve().parents[4] / fixture["height"]
        if not height_path.exists():
            self.skipTest("Tower source height map is outside the standalone gem checkout")
        self.assertEqual(hashlib.sha256(height_path.read_bytes()).hexdigest(),fixture["height_sha256"])
        image=np.asarray(Image.open(height_path),dtype=np.float32)/65535
        for case in fixture["cases"]:
            oracle=trace(image,case["packed_face"],case["ray_origin_direction"],
                         fixture["scale"],fixture["reference"],fixture["address"])
            self.assertEqual(oracle["status"],"checked")
            self.assertGreater(len(oracle["roots"]),1)
            self.assertAlmostEqual(oracle["nearest_t"],case["nearest_t"],places=6)
            profile=Profile(image,np.asarray(case["packed_face"],dtype=np.float32))
            profile.scale,profile.reference,profile.address=fixture["scale"],fixture["reference"],fixture["address"]
            hit,stats=self.evaluator.evaluate(profile,details(case["ray_origin_direction"]),7,(2,1,5))
            self.assertEqual(hit[0,0],1)
            # Accept a future fix that chooses the nearer root. Do not freeze
            # today's later-root behavior as a correctness requirement.
            self.assertTrue(any(abs(root["t"]-hit[0,1])<.00025 for root in oracle["roots"]))

    def test_reference_detects_tangent_without_sign_change(self):
        coords=(np.arange(4)+.5)/4
        image=(coords[None,:]-.5)*(coords[:,None]-.5)+.2
        direction=np.asarray([1,1,.2]);direction/=np.linalg.norm(direction)
        result=trace(image,face(uv_extent=2),np.r_[.3,.3,.15,direction],1,0,1,t_max=.7)
        self.assertEqual(result["status"],"checked")
        self.assertTrue(any(abs(root["t"]-.3*np.sqrt(2.04))<2e-5 for root in result["roots"]),result)


if __name__ == "__main__":
    unittest.main()

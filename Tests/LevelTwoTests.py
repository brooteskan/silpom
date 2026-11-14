"""Two-candidate selection, total budgets, descriptor capture and baseline guards."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"Tools"))
import numpy as np
from level_two import Native,pack

BINARY=Path(sys.argv.pop(1))


class Profile:
    def __init__(self):
        self.height=np.full((5,7),.75,np.float32)
        self.atlas=np.asarray(self.height.shape[::-1])
        self.scale,self.reference,self.address=.2,.5,1
        self.packed=np.asarray([[0,0,0,1,0,0,0,1,0,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1]],np.float32)


def detail():
    return {"ids":np.zeros(1,np.uint32),"rays":np.asarray([[.2,.2,1,0,0,-1]],np.float32)}


def constant(a,b,hit=100,score_a=0,score_b=0):
    logit=lambda value:np.log(value/(1-value))
    return [np.zeros((19,24),np.float32),np.zeros(24,np.float32),np.zeros((24,5),np.float32),
            np.asarray([logit(a),logit(b),hit,score_a,score_b],np.float32)]


class LevelTwo(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.native=Native(BINARY)

    def test_native_descriptor_locations_and_dimensions(self):
        p=Profile();d=detail()
        descriptors,valid=self.native.describe(p,d,4,2)
        self.assertTrue(valid.all())
        self.assertEqual(descriptors.shape,(1,21))
        start,span=descriptors[0,19:]
        samples=start+span*(np.arange(8,dtype=np.float32)+.5)/8
        expected=.75-((1-samples)/.2+.5)
        np.testing.assert_allclose(descriptors[0,:8],expected,atol=2e-6)
        self.assertEqual(descriptors[0,18],1)

    def test_second_seed_uses_one_retry(self):
        p=Profile();d=detail()
        # First seed is wrong and cannot finish in one evaluation. The second
        # is the exact flat-height intersection fraction .25.
        descriptor,_=self.native.describe(p,d,4,2)
        exact=(.95-descriptor[0,19])/descriptor[0,20]
        hit,s=self.native.evaluate2(p,d,4,(1,1,1),constant(.7,exact))
        self.assertEqual(hit[0,0],1)
        self.assertAlmostEqual(hit[0,1],.95,places=6)
        self.assertEqual(s[0,1],2)
        self.assertEqual(s[0,4],1)
        self.assertEqual(s[0,13],1)
        self.assertEqual(s[0,15],1)

    def test_zero_retry_runs_one_learned_candidate(self):
        p=Profile();d=detail()
        hit,s=self.native.evaluate2(p,d,4,(1,0,1),constant(.7,.25))
        self.assertEqual(hit[0,0],0)
        self.assertEqual(s[0,1],1)
        self.assertEqual(s[0,4],0)
        self.assertEqual(s[0,13],0)

    def test_fallback_shares_total_cap(self):
        p=Profile();d=detail()
        for cap in range(5):
            hit,s=self.native.evaluate2(p,d,4,(1,cap,1),constant(.7,.8))
            self.assertLessEqual(s[0,1],1+cap)
            self.assertLessEqual(s[0,4],cap)

    def test_low_confidence_retains_baseline(self):
        p=Profile();d=detail()
        a,sa=self.native.evaluate(p,d,4,(6,4,1))
        b,sb=self.native.evaluate2(p,d,4,(6,4,1),constant(.2,.8,hit=-100))
        np.testing.assert_array_equal(a,b)
        np.testing.assert_array_equal(sa[0,:12],sb[0,:12])
        self.assertEqual(sb[0,14],1)

    def test_calibration_does_not_move_seeds(self):
        from level_two import calibrate
        p=Profile();d=detail()
        descriptors,valid=self.native.describe(p,d,4,2)
        d["teacher"]=np.asarray([[1,.95]],np.float32)
        data=(descriptors[:,:19],np.ones(1,bool),np.asarray([.25],np.float32),descriptors[:,20],0)
        weights=constant(.7,.25)
        before=[v.copy() for v in weights]
        calibrate(self.native,p,4,data,d,weights)
        np.testing.assert_array_equal(before[0],weights[0])
        np.testing.assert_array_equal(before[1],weights[1])
        np.testing.assert_array_equal(before[2][:,:3],weights[2][:,:3])
        np.testing.assert_array_equal(before[3][:3],weights[3][:3])
        self.assertEqual(len(pack(weights)),605)


if __name__=="__main__":unittest.main()

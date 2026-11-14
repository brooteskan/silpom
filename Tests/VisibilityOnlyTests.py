"""Standalone decisions, unsupported-query rejection and compilation isolation."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"Tools"))
import numpy as np
from level_three import Native
from evaluate_visibility_only import classification,threshold

BINARY=Path(sys.argv.pop(1));DXC=Path(sys.argv.pop(1))


class Profile:
    def __init__(self):
        self.height=np.full((2,2048),.5,np.float32);self.atlas=self.height.shape[::-1]
        self.scale,self.reference,self.address=.2,.5,0
        self.packed=np.asarray([[0,0,0,1,0,0,0,1,0,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1]],np.float32)


class VisibilityOnly(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.native=Native(BINARY)

    def evaluate(self,cutoff=.6,model=4,steps=9,scale=.2):
        p=Profile();p.scale=scale
        q=np.asarray([[.2,.2,1,0,.01,.3,steps]],np.float32)
        w=[np.zeros((10,12),np.float32),np.zeros(12,np.float32),np.zeros(12,np.float32),np.zeros(1,np.float32)]
        return self.native.evaluate(p,q,np.zeros(1,np.uint32),model,3,cutoff,w)

    def test_uncertainty_is_a_final_binary_decision(self):
        a,s,_=self.evaluate(.6);b,ss,_=self.evaluate(.4)
        self.assertEqual(a[0,1],0);self.assertEqual(b[0,1],1)
        self.assertEqual(a[0,3],.5);self.assertFalse(s[:,5:7].any());self.assertFalse(ss[:,5:7].any())

    def test_saturated_descriptor_keeps_fixed_budget(self):
        _,s,_=self.evaluate()
        self.assertEqual(s[0,11],1);self.assertEqual(s[0,0],5)

    def test_unsupported_profile_rejected(self):
        with self.assertRaises(ValueError):self.evaluate(model=0)

    def test_negative_scale_rejected(self):
        with self.assertRaises(ValueError):self.evaluate(scale=-.2)

    def test_unsupported_schedule_rejected(self):
        for steps in (1,2,3,4):
            with self.assertRaises(ValueError):self.evaluate(steps=steps)

    def test_disabled_shadows_read_no_height(self):
        o,s,_=self.evaluate(steps=0)
        self.assertEqual(o[0,1],1);self.assertEqual(s[0,0],0)

    def test_calibration_and_binary_metrics(self):
        o=np.asarray([[1,0,0,.2],[1,0,0,.4],[1,1,1,.6],[1,1,1,.8]],np.float32)
        s=np.zeros((4,12),np.uint32);s[:,10]=1
        cutoff=threshold(o,s);self.assertGreater(cutoff,.4);self.assertLessEqual(cutoff,.6)
        metrics=classification(o,s);self.assertEqual(metrics['total_error_rate'],0)

    def test_four_probe_descriptor_can_hide_a_thin_blocker(self):
        p=Profile();p.address=1;p.height.fill(0);p.height[:,260]=1
        # At the first receiver, the ridge lies between physical-range probes.
        # The second receiver sees an empty region with identical probe values.
        q=np.asarray([[.125,.25,1,0,.04,0,9],[.5,.25,1,0,.04,0,9]],np.float32)
        o,s,x=self.native.evaluate(p,q,np.zeros(2,np.uint32),4,3,.5)
        np.testing.assert_array_equal(x[0],x[1])
        np.testing.assert_array_equal(o[:,2],[0,1])
        self.assertEqual(o[0,1],o[1,1]);self.assertTrue((s[:,10]==1).all())

    def test_directional_blocker_beyond_legacy_sixteen_texels(self):
        p=Profile();p.height=np.zeros((4,1000),np.float32);p.height[:,222:226]=1
        p.atlas=p.height.shape[::-1];p.scale=.05;p.reference=.05
        # One-metre UV basis: 1 mm texels, 5 cm relief. The blocker is
        # 23-26 texels away; a 16-texel directional schedule cannot reach it.
        light=[float(np.sqrt(1-.34**2)),0,.34]
        q=np.asarray([[.2,.2,*light,0,n] for n in (16,32,64)]+[[.2,.2,*light,.016,16]],np.float32)
        w=[np.zeros((10,12),np.float32),np.zeros(12,np.float32),np.zeros(12,np.float32),np.asarray([20],np.float32)]
        o,s,x=self.native.evaluate(p,q,np.zeros(len(q),np.uint32),4,3,.5,w)
        self.assertEqual(o[0,0],1,'Historical 16-texel baseline should miss this blocker')
        np.testing.assert_array_equal(o[:,2],[0,0,0,1])
        self.assertEqual(o[0,1],0,'Physical near descriptor must see the distant blocker')
        self.assertEqual(o[-1,1],1,'An explicit shorter light distance must exclude the blocker')
        self.assertFalse(s[:,5:7].any());self.assertLessEqual(s[:,0].max(),5)
        # Descriptor counts can change acquisition/approximation, never the
        # continuous teacher interval or its shadow label.
        self.assertEqual(len(set(o[:3,2])),1)

    def test_neural_compilation_contains_neither_marcher(self):
        with tempfile.TemporaryDirectory(prefix='silpom-visibility-') as folder:
            output=Path(folder)/'only.hlsl'
            subprocess.run([str(DXC),'-P','-Fi',str(output),'-D','SILPOM_NEURAL_VISIBILITY_ONLY=1',
                            str(Path(__file__).with_name('VisibilityGpu.hlsl'))],check=True,capture_output=True)
            source=output.read_text()
            self.assertIn('SilPomNeuralOnlyVisibility',source)
            self.assertNotIn('SilPomMarchLocalShadow',source);self.assertNotIn('SilPomReliefVisibility',source)


if __name__=='__main__':unittest.main()

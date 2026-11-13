"""Independent flat/ridge rendering oracles and the proxy no-traversal contract."""
import ctypes,math,sys,unittest
from pathlib import Path
import numpy as np
binary=Path(sys.argv.pop(1));root=Path(__file__).resolve().parents[1]
FP=np.ctypeslib.ndpointer(dtype=np.float32,flags='C_CONTIGUOUS');UP=np.ctypeslib.ndpointer(dtype=np.uint32,flags='C_CONTIGUOUS')
class Reference(unittest.TestCase):
    def render(self,height,q,samples=16):
        h,w=height.shape;maps=np.zeros((h,w,6),np.float32);maps[...,:3]=[.2,.3,.4];maps[...,3:5]=.5;maps[...,5]=.6
        face=np.array([[0,0,0,1,0,0,0,1,0,0,0,1,0,0,1,*([0,0,1]*3),0,0,1]],np.float32)
        dll=ctypes.CDLL(str(binary/'silpom_appearance_reference.dll'))
        dll.RenderAppearance.argtypes=[FP,ctypes.c_uint,ctypes.c_uint,ctypes.c_float,ctypes.c_float,FP,ctypes.c_uint,FP,FP,UP,ctypes.c_uint,ctypes.c_uint,FP]
        dll.RenderAppearance.restype=ctypes.c_bool
        q=np.ascontiguousarray(q,np.float32);output=np.empty((len(q),13),np.float32)
        self.assertTrue(dll.RenderAppearance(np.ascontiguousarray(height,np.float32),w,h,.05,.05,face,1,maps,q,np.zeros(len(q),np.uint32),len(q),samples,output))
        self.assertTrue(np.isfinite(output).all());return output
    def test_flat_pbr_oracle_and_footprint_convergence(self):
        q=[[.2,.2,0,0,1,.6,0,.8,.3,.1,.1,1,0]]
        outputs=[self.render(np.full((16,16),.75,np.float32),q,n)[0] for n in (4,16,64)]
        for o in outputs:
            np.testing.assert_allclose(o[:3],[0,0,1],atol=1e-6);self.assertAlmostEqual(o[3],.6,places=5)
            np.testing.assert_allclose(o[4:7],np.array([.2,.3,.4])*.8/math.pi,atol=2e-6)
            self.assertEqual(o[11],1);self.assertEqual(o[12],0)
        np.testing.assert_allclose(outputs[0],outputs[2],atol=2e-6)
    def test_ridge_visibility_changes_with_light_direction(self):
        height=np.zeros((64,256),np.float32);height[:,72:81]=1
        x=.99;z=math.sqrt(1-x*x)
        q=[[.25,.2,0,0,1,x,0,z,.3,.1,.1,1,0],[.25,.2,0,0,1,-x,0,z,.3,.1,.1,1,0]]
        out=self.render(height,q);self.assertLess(out[0,4],1e-6);self.assertGreater(out[1,4],.001)
    def test_far_depth_has_no_intersection_or_height_dependencies(self):
        text=(root/'Assets/Materials/Pipelines/MainPipeline/SilPOMHitDepthProxy.azsli').read_text()
        for forbidden in ('Heightfield.azsli','BlendedFace.azsli','SilPomIntersect','SilPomPredict','SampleLevel','SilPomRefine'):
            self.assertNotIn(forbidden,text)
        self.assertNotIn('SP_HIT_RECORD_WALL',text)
        source=(root/'Assets/Shaders/SilPOM/ProxyAppearance.azsli').read_text()
        for forbidden in ('m_normalMap','m_roughnessMap','SilPomFaceNormal','SilPomNeuralOnlyVisibility','SilPomIntersect'):
            self.assertNotIn(forbidden,source)
        self.assertIn('SilPomAppearanceLightGain',source)
if __name__=='__main__':unittest.main()

"""Independent filtering oracle and material-replacement source contract."""
import sys,unittest
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'Tools'))
import torch
from material_codec import sample,filtered

def bilinear(image,uv):
    h,w,_=image.shape;q=uv*np.array([w,h])-.5;i=np.floor(q).astype(int);t=q-i
    out=np.zeros(image.shape[2],np.float64)
    for y in (0,1):
        for x in (0,1):out+=image[(i[1]+y)%h,(i[0]+x)%w]*(t[0] if x else 1-t[0])*(t[1] if y else 1-t[1])
    return out

def trilinear(levels,uv,lod):
    low=int(np.floor(lod));high=min(len(levels)-1,low+1);f=lod-low
    return bilinear(levels[low],uv)*(1-f)+bilinear(levels[high],uv)*f

class Filtering(unittest.TestCase):
    def test_trilinear_repeat_seams_and_mip_boundaries(self):
        rng=np.random.default_rng(41);levels=[rng.random((h,w,8)).astype(np.float32) for w,h in ((13,9),(6,4),(3,2),(1,1))]
        uv=np.asarray([[-.01,.5],[.9999,1.0001],[0,0],[.3,.7]],np.float32);lod=np.asarray([0,.5,1,3],np.float32)
        actual=sample([torch.from_numpy(a) for a in levels],torch.from_numpy(uv),torch.from_numpy(lod)).numpy()
        expected=np.stack([trilinear(levels,u,float(l)) for u,l in zip(uv,lod)])
        np.testing.assert_allclose(actual,expected,atol=2e-6)

    def test_oblique_filtered_features_against_independent_sum(self):
        rng=np.random.default_rng(7);levels=[rng.random((h,w,8)).astype(np.float32) for w,h in ((13,9),(6,4),(3,2),(1,1))]
        uv=torch.tensor([[.2,.5],[-.02,1.01]]);major=torch.tensor([32.,64.]);minor=torch.tensor([4.,8.])
        axis=torch.tensor([[1.,0.],[.6,.8]]);q=(uv,major,minor,axis,None)
        actual=filtered([torch.from_numpy(a) for a in levels],q,True).numpy();expected=[]
        for u,a,b,d in zip(uv.numpy(),major.numpy(),minor.numpy(),axis.numpy()):
            n=min(8,max(1,int(np.ceil(a/b))));lod=np.clip(np.log2(b)-2,0,len(levels)-1)
            expected.append(sum(trilinear(levels,u+d*((i+.5)/n-.5)*a/np.array([6656,6144]),lod) for i in range(n))/n)
        np.testing.assert_allclose(actual,expected,atol=2e-6)

    def test_production_decoder_never_samples_replaced_maps(self):
        root=Path(__file__).resolve().parents[1]/'Assets'
        source=(root/'Shaders/SilPOM/LearnedMaterialSurface.azsli').read_text()
        for forbidden in ('EvaluateSurface_StandardPBR','m_baseColorMap','m_normalMap','m_roughnessMap','SilPomIntersect','SilPomMarch'):
            self.assertNotIn(forbidden,source)
        variant=(root/'Materials/Pipelines/MainPipeline/SilPOMHitForwardLearnedMaterial.azsli').read_text()
        self.assertIn('#define SILPOM_LEARNED_MATERIAL 1',variant)
        self.assertIn('#define SILPOM_NEURAL_VISIBILITY_ONLY 1',variant)

if __name__=='__main__':unittest.main()

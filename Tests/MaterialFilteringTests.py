"""Independent filtering oracle and material-replacement source contract."""
import sys,unittest,tempfile,hashlib,struct
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'Tools'))
import torch
from material_codec import sample,filtered
from relief_appearance import feature_sample,features
from frozen_bundle import HEADER,CONTRACTS,source_fingerprint,verify_frozen_header,read_verified_bundle

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
    def test_frozen_input_cannot_be_relabelled_with_new_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)
            for name in ('basecolor.png','normal.png','roughness.png','height.png','mesh.json'):(p/name).write_bytes(name.encode())
            for kind in ('material','appearance'):
                mesh=p/'mesh.json' if kind=='appearance' else None
                original=source_fingerprint(p,mesh);header=(*CONTRACTS[kind],original)
                verify_frozen_header(header,kind,original)
                for changed in (('basecolor.png','normal.png','roughness.png') if mesh is None else ('basecolor.png','normal.png','roughness.png','height.png','mesh.json')):
                    old=(p/changed).read_bytes();(p/changed).write_bytes(old+b'changed')
                    current=source_fingerprint(p,mesh)
                    with self.assertRaisesRegex(ValueError,'Stale frozen'):verify_frozen_header(header,kind,current)
                    # Public loader rejects provenance before decoding any
                    # frozen feature payload or training a replacement head.
                    (p/'stale.bundle').write_bytes(HEADER.pack(*header))
                    with self.assertRaisesRegex(ValueError,'Stale frozen'):read_verified_bundle(p/'stale.bundle',kind,current)
                    (p/changed).write_bytes(old)

    def test_proxy_query_preserves_oblique_filtered_appearance_during_motion(self):
        # Independent finite local-plane ray solve. Spatially varying material
        # features expose the double-shift error that scalar depth checks miss.
        rng=np.random.default_rng(118);levels=[rng.random((h,w,8)).astype(np.float32) for w,h in ((13,9),(6,4),(3,2),(1,1))]
        samples=[];wrong=[];truth=[]
        for z in (.35,.5,.8):
            for u in np.linspace(.1,.9,41):
                uv=np.array([u,.47]);V=np.array([np.sqrt(1-z*z),0,z]);D=np.array([.2,.1,.97]);h=.05*(.5+.3*np.sin(u*17)-.05)
                base=np.array([2*uv[0]+.4*uv[1],1.7*uv[1],0]);resolved=base+V*(h*D[2]/z)
                hitDelta=V*(h*D[2]/z)-D*h;hitUv=uv+np.array([(hitDelta[0]-.4*hitDelta[1]/1.7)/2,hitDelta[1]/1.7])
                restored=resolved-V*(resolved[2]/z);restoredUv=np.array([(restored[0]-.4*restored[1]/1.7)/2,restored[1]/1.7])
                truth.append(trilinear(levels,uv,.8));samples.append(trilinear(levels,restoredUv,.8));wrong.append(trilinear(levels,hitUv,.8))
        np.testing.assert_allclose(samples,truth,atol=1e-10)
        self.assertGreater(float(np.abs(np.asarray(wrong)-truth).mean()),.003)

    def test_proxy_runtime_and_directional_enablement_contract(self):
        root=Path(__file__).resolve().parents[1]
        source=(root/'Assets/Shaders/SilPOM/ProxyAppearance.azsli').read_text()
        self.assertIn('SilPomRestoreProxyQuery',source);self.assertIn('SilPomAcquireAppearance(query.uv',source)
        self.assertIn('GetMaterialTextureSampler(params.m_silpomAppearanceColorSampler),query.uv,',source)
        self.assertNotIn('SilPomAcquireAppearance(geo.uvs',source)
        lighting=(root/'Assets/Materials/Types/SilPOMMesh/SilPOMMesh_DirectionalLighting.azsli').read_text()
        self.assertIn('m_silpomNeuralDirectionalVisibility?16u:0u',lighting)
        self.assertIn('m_silpomProxyDirectionalResponse,params)',lighting)
    def test_proxy_features_clamp_atlas_borders(self):
        rng=np.random.default_rng(17);levels=[rng.random((h,w,8)).astype(np.float32) for w,h in ((13,9),(6,4),(3,2),(1,1))]
        uv=np.array([[-.2,.3],[1.1,1.01],[0,0],[.3,.7]],np.float32);lod=np.array([0,.7,1.8,3],np.float32)
        def clamp_bilinear(image,u):
            h,w,_=image.shape;q=u*np.array([w,h])-.5;i=np.floor(q).astype(int);f=q-i
            return sum(image[np.clip(i[1]+y,0,h-1),np.clip(i[0]+x,0,w-1)]*(f[0] if x else 1-f[0])*(f[1] if y else 1-f[1]) for y in (0,1) for x in (0,1))
        expected=[]
        for u,l in zip(uv,lod):
            low=int(l);high=min(len(levels)-1,low+1);f=float(l-low)
            expected.append(clamp_bilinear(levels[low],u)*(1-f)+clamp_bilinear(levels[high],u)*f)
        actual=feature_sample([torch.from_numpy(v) for v in levels],torch.from_numpy(uv),torch.from_numpy(lod)).numpy()
        np.testing.assert_allclose(actual,np.stack(expected),atol=2e-6)

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

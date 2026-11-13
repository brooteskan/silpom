"""Frozen Torch fixtures and actual shipping shader evaluated on D3D12.

No Torch installation needed. Optional binary-dir runs both GPU shader models.
"""
import hashlib,json,re,subprocess,sys,tempfile
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[1]
KINDS={2:'Field',3:'Caps4',4:'Caps8'}

def read_weights():
    text=(ROOT/'Assets/Shaders/SilPOM/DirectionalEstimatorWeights.azsli').read_text()
    weights={}
    for mode,name in KINDS.items():
        result=[]
        for suffix,shape in (('W0',(32,20)),('B0',(32,)),('W1',(-1,32)),('B1',(-1,))):
            body=re.search(r'SilPomDirectional'+name+suffix+r'\[\d+\] = \{([^}]+)',text).group(1)
            result.append(np.array([float(v.rstrip('f')) for v in body.replace('\n','').split(',') if v.strip()],np.float32).reshape(shape))
        weights[mode]=result
    return weights

def sigmoid(x):return 1/(1+np.exp(-np.clip(x,-32,32)))

def predict(x,light,reach,mode,weights):
    w,b,v,c=weights[mode];raw=np.maximum(x@w.T+b,0)@v.T+c
    light=light/np.linalg.norm(light,axis=1,keepdims=True)
    lo=np.where(reach>0,np.where(reach<=.1,0,1),3);hi=np.where(reach>0,lo+1,3)
    blend=np.where(reach>0,np.clip((reach-np.where(lo==0,.025,.1))/np.where(lo==0,.075,.2),0,1),0)
    row=np.arange(len(x))
    if mode==2:
        # Explicit canonical frame fixture freezes direction ordering.
        directions=np.asarray(json.loads((ROOT/'Tests/Reference/DirectionalEstimator.json').read_text())['directions'],np.float32)
        nearest=np.argmax(light@directions.T,axis=1)
        field=np.minimum.accumulate(sigmoid(raw.reshape(-1,4,32)),axis=1)
        a=field[row,lo,nearest];b=field[row,hi,nearest]
        value=a+(b-a)*blend
    else:
        count=4 if mode==3 else 8
        axes=raw[:,:count*3].reshape(-1,count,3)
        axes=axes/np.maximum(np.linalg.norm(axes,axis=2,keepdims=True),1e-6)
        cosine=np.sort(sigmoid(raw[:,count*3:].reshape(-1,count,4)),axis=-1)[:,:,::-1]
        a=cosine[row,:,lo];b=cosine[row,:,hi]
        threshold=a+(b-a)*blend[:,None]
        margin=(np.einsum('nkd,nd->nk',axes,light)-threshold).max(axis=1)
        value=sigmoid(-32*margin)
    return np.where(light[:,2]<=0,0,value).astype(np.float32)

def main():
    fixture=json.loads((ROOT/'Tests/Reference/DirectionalEstimator.json').read_text())
    assert hashlib.sha256((ROOT/'Assets/Shaders/SilPOM/DirectionalEstimatorWeights.azsli').read_bytes()).hexdigest()==fixture['weights_sha256'],'Frozen neural weights changed'
    weights=read_weights();x=np.asarray(fixture['x'],np.float32);light=np.asarray(fixture['light'],np.float32)
    reach=np.asarray(fixture['reach'],np.float32)
    for mode in KINDS:
        pred=predict(x,light,reach,mode,weights)
        np.testing.assert_allclose(pred,fixture['expected'][str(mode)],atol=3e-5,rtol=3e-5)
        previous=np.ones(len(x),np.float32)
        for band in (.025,.05,.1,.2,.3,0):
            current=predict(x,light,np.full(len(x),band,np.float32),mode,weights)
            assert np.all(current<=previous+2e-6),'Reach must be monotone'
            assert np.all((current>=0)&(current<=1))
            previous=current
    print('Three shipping neural heads match frozen Torch fixtures; reach monotonicity and hemisphere bounds passed.')
    if len(sys.argv)>1:
        binary=Path(sys.argv[1]);queries=[];expected=[]
        for mode in KINDS:
            for band in (0,.025,.05,.1,.2,.3):
                for steps in (0,16):
                    r=np.full(len(x),band,np.float32)
                    queries.append(np.column_stack((x,light,r,np.full(len(x),steps),np.full(len(x),mode))).astype(np.float32))
                    expected.append(np.ones(len(x),np.float32) if not steps else predict(x,light,r,mode,weights))
        with tempfile.TemporaryDirectory(prefix='silpom-directional-') as folder:
            folder=Path(folder);q=folder/'requests.f32';e=folder/'expected.f32'
            np.concatenate(queries).tofile(q);np.concatenate(expected).tofile(e)
            for sm in ('6_2','6_7'):
                subprocess.run([str(binary/'Release/silpom_directional_estimator_gpu_tests.exe'),str(binary/('directional-estimator-'+sm+'.dxil')),str(q),str(e)],check=True)

if __name__=='__main__':main()

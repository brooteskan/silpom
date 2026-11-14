"""Level 5A: fit PBR-compatible appearance to filtered detailed-relief rendering.

Offline tracing is confined to appearance_reference.dll. Neither runtime
decoder performs intersection, descriptor probes, refinement or recovery.
"""
import argparse,ctypes,hashlib,json,os,struct
from pathlib import Path
os.environ.setdefault('OMP_NUM_THREADS','4')
import numpy as np
from PIL import Image
import torch
from material_codec import source_mips,sample,HEADER
from train_welded_tower import Profile
from level_one import Evaluator,FP,UP,U,F
torch.set_num_threads(4)

def load_wall(project,binary):
    mesh_path=project/'Assets/SilPOMPlanarAB/IrishFieldstoneTowerOpenShaft/tower-open-shaft-no-landings-silpom.silpom.json'
    mesh=json.loads(mesh_path.read_bytes());faces=sorted(mesh['faces'],key=lambda f:f['stable_id'])
    xyz=np.ascontiguousarray([[mesh['vertices'][str(v)] for v in f['vertices']] for f in faces],np.float32)
    vertices=np.ascontiguousarray([f['vertices'] for f in faces],np.uint32)
    tagged=np.ascontiguousarray([f['region']!=0 for f in faces],np.uint32);directions=np.empty_like(xyz)
    native=Evaluator(binary)
    assert native.teacher.BlendDirections(xyz,vertices,tagged,len(faces),directions)
    keep=np.asarray([f['region']!=0 and f['profile']==1 for f in faces])
    wall=project/'Assets/SilPOMPlanarAB/IrishFieldstoneTowerWelded/wall'
    p=Profile({'tiling':[1,1],'offset':[0,0],'scale_metres':.05,'reference':.05,'address':'clamp'},wall/'height.png',
        xyz[keep],np.asarray([f['uv'] for f in faces])[keep],directions[keep])
    return p,wall,mesh_path

def queries(p,count,seed):
    rng=np.random.default_rng(seed);area=np.linalg.norm(np.cross(p.base[:,1],p.base[:,2]),axis=1)
    ids=rng.choice(len(p.packed),count,p=area/area.sum()).astype(np.uint32)
    b=rng.uniform(.08,.92,(count,3));b/=b.sum(1,keepdims=True)
    uv=np.einsum('ni,nij->nj',b,p.uv[ids])
    def direction(low):
        z=rng.uniform(low,1,count);a=rng.uniform(-np.pi,np.pi,count)
        return np.column_stack((np.cos(a)*np.sqrt(1-z*z),np.sin(a)*np.sqrt(1-z*z),z))
    view=direction(.35);light=direction(.03)
    major=2**rng.uniform(3,13,count);minor=np.maximum(1,major/2**rng.uniform(0,3,count))
    angle=rng.uniform(-np.pi,np.pi,count);axis=np.column_stack((np.cos(angle),np.sin(angle)))
    reach=rng.choice([0,.05,.1,.3],count)
    q=np.ascontiguousarray(np.column_stack((uv,view,light,reach,major,minor,axis)),np.float32)
    metric=np.column_stack((np.log2(np.linalg.norm(p.du[ids],axis=1))/16,
        np.log2(np.linalg.norm(p.dv[ids],axis=1))/16,
        np.einsum('ni,ni->n',p.du[ids],p.dv[ids])/np.linalg.norm(p.du[ids],axis=1)/np.linalg.norm(p.dv[ids],axis=1)))
    footprint=np.column_stack((np.minimum(12,np.log2(major))/13,np.minimum(12,np.log2(minor))/13,np.cos(2*angle),np.sin(2*angle)))
    return q,ids,np.ascontiguousarray(np.column_stack((footprint,metric)),np.float32)

def reference(p,binary,maps,q,ids,samples):
    dll=ctypes.CDLL(str((binary/'silpom_appearance_reference.dll').resolve()))
    dll.RenderAppearance.argtypes=[FP,U,U,F,F,FP,U,FP,FP,UP,U,U,FP];dll.RenderAppearance.restype=ctypes.c_bool
    out=np.empty((len(q),13),np.float32)
    assert dll.RenderAppearance(p.height,*p.atlas,p.scale,p.reference,p.packed,len(p.packed),maps,q,ids,len(q),samples,out)
    assert np.isfinite(out).all()
    return out

class Atlas:
    def __init__(self,levels):
        self.shapes=[v.shape for v in levels];self.offsets=np.r_[0,np.cumsum([v.shape[0]*v.shape[1] for v in levels])]
        self.embedding=torch.nn.Embedding.from_pretrained(torch.cat([v.reshape(-1,8) for v in levels]),freeze=False,sparse=True)
    def __len__(self):return len(self.shapes)
    def shape(self,i):return self.shapes[i]
    def tap(self,i,x,y):
        h,w,_=self.shapes[i];v=self.embedding(y.clamp(0,h-1)*w+x.clamp(0,w-1)+int(self.offsets[i]))
        return v+(v.clamp(0,1).mul(255).round().div(255)-v).detach()
    def export(self):return [self.embedding.weight.detach()[self.offsets[i]:self.offsets[i+1]].reshape(s).clamp(0,1).mul(255).round()/255 for i,s in enumerate(self.shapes)]

def feature_sample(atlas,uv,lod):
    result=torch.zeros((len(uv),8));lo=lod.floor().long();hi=(lo+1).clamp(max=len(atlas)-1);blend=lod-lo
    for ids,weight in ((lo,1-blend),(hi,blend)):
        for i in range(len(atlas)):
            mask=ids==i
            if not mask.any():continue
            h,w,_=atlas.shape(i) if isinstance(atlas,Atlas) else atlas[i].shape
            q=uv[mask]*torch.tensor([w,h])-.5;cell=q.floor().long();f=q-cell
            def tap(x,y):
                xx,yy=(cell[:,0]+x).clamp(0,w-1),(cell[:,1]+y).clamp(0,h-1)
                return atlas.tap(i,xx,yy) if isinstance(atlas,Atlas) else atlas[i][yy,xx]
            value=tap(0,0)*(1-f[:,:1])*(1-f[:,1:])+tap(1,0)*f[:,:1]*(1-f[:,1:])+tap(0,1)*(1-f[:,:1])*f[:,1:]+tap(1,1)*f[:,:1]*f[:,1:]
            result[mask]+=value*weight[mask,None]
    return result

def features(atlas,q):
    major,minor,axis=q[:,9],q[:,10],q[:,11:13];taps=(major/minor).ceil().long().clamp(1,8)
    lod=(minor.log2()-3).clamp(0,len(atlas)-1);result=torch.zeros((len(q),8))
    for i in range(8):
        mask=i<taps
        if not mask.any():continue
        offset=((i+.5)/taps[mask]-.5)*major[mask]
        uv=q[mask,:2]+axis[mask]*offset[:,None]/torch.tensor([6656,6144])
        result[mask]+=feature_sample(atlas,uv,lod[mask])/taps[mask,None]
    return result*2-1

def network(inputs,outputs):
    model=torch.nn.Sequential(torch.nn.Linear(inputs,16),torch.nn.ReLU(),torch.nn.Linear(16,outputs))
    with torch.no_grad():model[2].weight.mul_(.01);model[2].bias.zero_()
    return model

def shade(surface,view,light,color):
    xy=(surface[:,:2]*2-1)*.95
    n=torch.cat((xy,(1-xy.square().sum(1,keepdim=True)).clamp(min=.01).sqrt()),1)
    n=torch.nn.functional.normalize(n,dim=1);rough=.08+.92*surface[:,2]
    nv=(n*view).sum(1).clamp(min=1e-4);nl=(n*light).sum(1).clamp(min=0)
    half=torch.nn.functional.normalize(view+light,dim=1);nh=(n*half).sum(1).clamp(min=0);vh=(view*half).sum(1).clamp(min=0)
    a=rough**4;d=a/(np.pi*(nh.square()*(a-1)+1).square());k=(rough+1).square()/8
    g=nv/(nv*(1-k)+k)*nl/(nl*(1-k)+k);spec=d*g*(.04+.96*(1-vh)**5)/(4*nv)
    return color*nl[:,None]/np.pi,spec,n,rough

def run_models(atlas,surface,light,q,context):
    z=features(atlas,q);v=q[:,2:5];l=q[:,5:8]
    s=torch.sigmoid(surface(torch.cat((z,v,context),1)).clamp(-16,16))
    gain=2*torch.sigmoid(light(torch.cat((z,v,l,context,q[:,8:9]/.3),1)).clamp(-16,16))
    return s,gain

def pack(model):
    return np.concatenate([model[0].weight.detach().numpy().ravel(),model[0].bias.detach().numpy(),
        model[2].weight.detach().numpy().T.ravel(),model[2].bias.detach().numpy()]).astype('<f4')

def main():
    cli=argparse.ArgumentParser(description=__doc__);cli.add_argument('project',type=Path);cli.add_argument('binary',type=Path);cli.add_argument('output',type=Path)
    cli.add_argument('--queries',type=int,default=32768);cli.add_argument('--steps',type=int,default=2000);cli.add_argument('--samples',type=int,default=16)
    a=cli.parse_args();a.output.mkdir(parents=True,exist_ok=True);torch.manual_seed(55191)
    p,wall,mesh=load_wall(a.project,a.binary)
    print('Loading canonical material and source height',flush=True);teacher=source_mips(wall)
    maps=np.ascontiguousarray(teacher[0].numpy());del teacher[0:3]
    # Start from mip-reduced material plus relief height and slope statistics.
    levels=[]
    height=Image.open(wall/'height.png').convert('F')
    for t in teacher:
        h,w,_=t.shape;hm=np.asarray(height.resize((w,h),Image.Resampling.BOX),np.float32)/65535
        gy,gx=np.gradient(hm) if min(h,w)>1 else (np.zeros_like(hm),np.zeros_like(hm))
        data=np.concatenate((hm[...,None],(np.arctan(gx*6656*.05)/np.pi+.5)[...,None],
            (np.arctan(gy*6144*.05)/np.pi+.5)[...,None],t[...,3:6].numpy(),t[...,:1].numpy(),t[...,1:2].numpy()),2)
        levels.append(torch.from_numpy(data.astype(np.float32)))
    atlas=Atlas(levels);surface=network(18,3);light=network(22,2)
    sets=[]
    for label,count,seed in (('train',a.queries,7721),('held-out',8192,99431)):
        q,ids,context=queries(p,count,seed);print('Rendering '+label+' detailed relief',flush=True)
        data=reference(p,a.binary,maps,q,ids,a.samples)
        np.savez(a.output/(label+'.npz'),q=q,ids=ids,context=context,reference=data)
        valid=(data[:,11]>.5)&(data[:,12]==0)
        print(label+': usable '+str(int(valid.sum()))+'/'+str(count),flush=True)
        sets.append(tuple(torch.from_numpy(v[valid]) for v in (q,context,data)))
    del maps,teacher
    optimizer=torch.optim.Adam(list(surface.parameters())+list(light.parameters()),lr=.0015)
    sparse=torch.optim.SparseAdam(atlas.embedding.parameters(),lr=.004)
    g=torch.Generator().manual_seed(15211);q,context,target=sets[0]
    for step in range(a.steps):
        ids=torch.randint(len(q),(512,),generator=g);qq,cc,tt=q[ids],context[ids],target[ids]
        s,gain=run_models(atlas,surface,light,qq,cc);diff,spec,n,rough=shade(s,qq[:,2:5],qq[:,5:8],tt[:,8:11])
        # Fit filtered V*f_r jointly. The two bounded corrections retain
        # diffuse/specular covariance with local visibility and relief normals.
        rendered=torch.cat((diff*gain[:,:1],spec[:,None]*gain[:,1:2]),1)
        desired=tt[:,4:8];loss=(torch.log1p(rendered*8)-torch.log1p(desired*8)).square().mean()
        loss+=.015*(n-torch.nn.functional.normalize(tt[:,:3],dim=1)).square().mean()+.01*(rough-tt[:,3]).square().mean()
        optimizer.zero_grad(set_to_none=True);sparse.zero_grad(set_to_none=True);loss.backward();optimizer.step();sparse.step()
        if step%250==0:
            with torch.no_grad():atlas.embedding.weight.clamp_(0,1)
            print(step,float(loss.detach()),flush=True)
    exported=atlas.export();q,context,target=sets[1];errors=[];flatErrors=[]
    with torch.no_grad():
        for start in range(0,len(q),512):
            qq,cc,tt=q[start:start+512],context[start:start+512],target[start:start+512]
            s,gain=run_models(exported,surface,light,qq,cc);diff,spec,_,_=shade(s,qq[:,2:5],qq[:,5:8],tt[:,8:11])
            predicted=torch.cat((diff*gain[:,:1],spec[:,None]*gain[:,1:2]),1)
            errors.append((predicted-tt[:,4:8]).abs().numpy())
            flat=torch.zeros((len(qq),3));flat[:,:2]=.5;flat[:,2]=(tt[:,3]-.08)/.92
            fd,fs,_,_=shade(flat,qq[:,2:5],qq[:,5:8],tt[:,8:11]);flatErrors.append((torch.cat((fd,fs[:,None]),1)-tt[:,4:8]).abs().numpy())
    errors=np.concatenate(errors);flatErrors=np.concatenate(flatErrors)
    digest=hashlib.sha256(b''.join((wall/n).read_bytes() for n in ('basecolor.png','normal.png','roughness.png','height.png'))+mesh.read_bytes()).digest()
    weights=np.concatenate((pack(surface),pack(light)));assert len(weights)==757
    payload=b''.join(v.mul(255).round().byte().numpy().tobytes() for v in exported)
    header=HEADER.pack(b'SILPAPP5',1,832,768,len(exported),6656,6144,len(weights),digest)
    (a.output/'wall.l5app').write_bytes(header+weights.tobytes()+payload)
    report={'architecture':{'surface':[18,16,3],'light':[22,16,2]},'features':8,'feature_base':[832,768],'mips':len(exported),
        'feature_bytes':len(payload),'weights':len(weights),'samples_per_pixel':a.samples,'training_queries':a.queries,'steps':a.steps,
        'held_out_usable_queries':len(errors),'mae_diffuse_rgb_specular':errors.mean(0).tolist(),'p95_abs':np.quantile(errors,.95,axis=0).tolist(),
        'flat_pbr_mae':flatErrors.mean(0).tolist(),'source_sha256':digest.hex(),'benchmark_performed':False,
        'teacher':'detailed affine displacement trace + source material + dense local visibility + GGX/Schlick; finite stratified pixel supersampling'}
    (a.output/'quality.json').write_text(json.dumps(report,indent=2)+'\n');print(report,flush=True)

if __name__=='__main__':main()

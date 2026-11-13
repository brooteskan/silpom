"""Level 4A: learned eight-channel mip atlas and 12->16->6 material decoder."""
import argparse,hashlib,json,os,struct
from pathlib import Path
os.environ.setdefault('OMP_NUM_THREADS','4')
import numpy as np
from PIL import Image
import torch
torch.set_num_threads(4)
HEADER=struct.Struct('<8s7I32s')

def source_mips(folder):
    color=np.asarray(Image.open(folder/'basecolor.png'),np.float32)/255
    color=np.where(color<=.04045,color/12.92,((color+.055)/1.055)**2.4)
    normal=np.asarray(Image.open(folder/'normal.png'),np.float32)/127.5-1
    rough=np.asarray(Image.open(folder/'roughness.png'),np.float32)[...,:1]/255
    levels=[]
    while True:
        # PC normal input uses the pipeline's swapped G/R convention.
        levels.append(torch.from_numpy(np.concatenate((color,normal[...,[1,0]]*.5+.5,rough),axis=2)))
        if color.shape[:2]==(1,1):break
        hh,ww=max(1,color.shape[0]//2),max(1,color.shape[1]//2)
        def resize(a):
            return np.stack([np.asarray(Image.fromarray(a[...,k],'F').resize((ww,hh),Image.Resampling.BOX)) for k in range(a.shape[2])],axis=2)
        color=resize(color);rough=resize(rough);normal=resize(normal)
        normal/=np.maximum(np.linalg.norm(normal,axis=2,keepdims=True),1e-8)
    return levels

class Atlas:
    def __init__(self,teacher):
        self.shapes=[tuple(t.shape) for t in teacher[2:]]
        self.offsets=np.r_[0,np.cumsum([h*w for h,w,_ in self.shapes])]
        initial=[]
        for target in teacher[2:]:
            extra=torch.stack((target[...,:3].mean(2),target[...,3:5].square().sum(2)),2)
            initial.append(torch.cat((target,extra),2).reshape(-1,8))
        self.embedding=torch.nn.Embedding.from_pretrained(torch.cat(initial),freeze=False,sparse=True)
    def __len__(self):return len(self.shapes)
    def shape(self,i):return (*self.shapes[i][:2],8)
    def tap(self,i,x,y):
        h,w,_=self.shape(i)
        v=self.embedding(y%h*w+x%w+int(self.offsets[i]))
        return v+(v.clamp(0,1).mul(255).round().div(255)-v).detach()
    def export(self):
        return [self.embedding.weight.detach()[self.offsets[i]:self.offsets[i+1]].reshape(self.shape(i)).clamp(0,1).mul(255).round().byte().float()/255 for i in range(len(self))]


def sample(levels,uv,lod):
    result=torch.zeros((len(uv),(8 if isinstance(levels,Atlas) else levels[0].shape[2])),dtype=torch.float32)
    lo=torch.floor(lod).long().clamp(0,len(levels)-1);hi=(lo+1).clamp(max=len(levels)-1);f=lod-lo
    for ids,blend in ((lo,1-f),(hi,f)):
        for i in range(len(levels)):
            image=None if isinstance(levels,Atlas) else levels[i]
            shape=levels.shape(i) if image is None else image.shape
            mask=ids==i
            if not mask.any():continue
            q=uv[mask]*torch.tensor([shape[1],shape[0]])-.5
            cell=q.floor().long();t=q-cell
            def tap(x,y):return levels.tap(i,cell[:,0]+x,cell[:,1]+y) if image is None else image[(cell[:,1]+y)%shape[0],(cell[:,0]+x)%shape[1]]
            v=tap(0,0)*(1-t[:,:1])*(1-t[:,1:])+tap(1,0)*t[:,:1]*(1-t[:,1:])+tap(0,1)*(1-t[:,:1])*t[:,1:]+tap(1,1)*t[:,:1]*t[:,1:]
            result[mask]+=v*blend[mask,None]
    return result

def queries(n,generator):
    uv=torch.rand((n,2),generator=generator)*1.5-.25
    major=2**(torch.rand(n,generator=generator)*13-1)
    ratio=2**(torch.rand(n,generator=generator)*6)
    minor=torch.maximum(torch.maximum(major/ratio,major/8),torch.ones(n))
    angle=torch.rand(n,generator=generator)*np.pi
    axis=torch.stack((angle.cos(),angle.sin()),1)
    footprint=torch.stack((major.clamp(min=1).log2()/13,minor.log2()/13,(2*angle).cos(),(2*angle).sin()),1)
    return uv,major,minor,axis,footprint

def filtered(levels,q,latent=False):
    uv,major,minor,axis,_=q
    taps=torch.ceil(torch.maximum(major/minor,torch.ones_like(major))).long().clamp(1,8)
    lod=minor.log2().clamp(min=0,max=12)
    if latent:lod=(lod-2).clamp(min=0,max=len(levels)-1)
    result=torch.zeros((len(uv),(8 if isinstance(levels,Atlas) else levels[0].shape[2])))
    for i in range(8):
        m=i<taps
        if not m.any():continue
        shift=((i+.5)/taps[m]-.5)*major[m]
        coords=uv[m]+axis[m]*shift[:,None]/torch.tensor([6656,6144])
        result[m]+=sample(levels,coords,lod[m])/taps[m,None]
    return result

def pack(weights):return np.concatenate((weights[0].T.ravel(),weights[1],weights[2].ravel(),weights[3])).astype('<f4')

def main():
    cli=argparse.ArgumentParser(description=__doc__)
    cli.add_argument('source',type=Path);cli.add_argument('output',type=Path)
    cli.add_argument('--steps',type=int,default=3500);cli.add_argument('--batch',type=int,default=1024)
    a=cli.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    torch.manual_seed(4417);g=torch.Generator().manual_seed(43191)
    print('Loading source mip chains',flush=True);teacher=source_mips(a.source)
    # Initialize useful surface codes, then jointly optimize atlas values and
    # decoder through exactly the interpolation/filtering used at runtime.
    latent=Atlas(teacher)
    w0=torch.nn.Parameter(torch.zeros(12,16));b0=torch.nn.Parameter(torch.ones(16)*.1)
    w1=torch.nn.Parameter(torch.zeros(16,6));b1=torch.nn.Parameter(torch.zeros(6))
    with torch.no_grad():
        w0[:6,:6]=torch.eye(6);b0[:6]=1;w1[:6]=torch.eye(6)*.5
        w0[6:]=torch.randn(6,16)*.01
    optimizer=torch.optim.Adam([w0,b0,w1,b1],lr=.001)
    sparse=torch.optim.SparseAdam(latent.embedding.parameters(),lr=.005)
    for step in range(a.steps):
        q=queries(a.batch,g)
        with torch.no_grad():target=filtered(teacher,q)
        # Straight-through UNORM8 quantization is part of the trained encoding.
        levels=latent
        x=torch.cat((filtered(levels,q,True)*2-1,q[-1]),1)
        pred=torch.relu(x@w0+b0)@w1+b1
        loss=((pred-target)**2*torch.tensor([1,1,1,2,2,1])).mean()
        optimizer.zero_grad(set_to_none=True);sparse.zero_grad(set_to_none=True);loss.backward();optimizer.step();sparse.step()
        if step%250==0:
            with torch.no_grad():latent.embedding.weight.clamp_(0,1)
            print(step,float(loss.detach()),flush=True)
    weights=[v.detach().numpy() for v in (w0,b0,w1,b1)]
    levels=latent.export()
    held=torch.Generator().manual_seed(89451);errors=[];angles=[];rows=[]
    for _ in range(32):
        q=queries(1024,held);truth=filtered(teacher,q)
        with torch.no_grad():
            x=torch.cat((filtered(levels,q,True)*2-1,q[-1]),1)
            pred=(torch.relu(x@w0+b0)@w1+b1).clamp(0,1)
        errors.append((pred-truth).abs().numpy());rows.append(torch.cat((x,pred),1).numpy())
        def normals(v):
            xy=v[:,3:5]*2-1;return torch.cat((xy,(1-xy.square().sum(1,keepdim=True)).clamp(min=0).sqrt()),1)
        p,t=normals(pred),normals(truth)
        angles.append((torch.nn.functional.cosine_similarity(p,t,dim=1).clamp(-1,1).acos()*180/np.pi).numpy())
    errors=np.concatenate(errors);angles=np.concatenate(angles)
    digest=hashlib.sha256(b''.join((a.source/n).read_bytes() for n in ('basecolor.png','normal.png','roughness.png'))).digest()
    payload=b''.join(v.mul(255).round().byte().numpy().tobytes() for v in levels)
    header=HEADER.pack(b'SILPMAT4',1,levels[0].shape[1],levels[0].shape[0],len(levels),6656,6144,310,digest)
    (a.output/'wall.l4mat').write_bytes(header+pack(weights).tobytes()+payload)
    np.savez(a.output/'model.npz',w0=weights[0],b0=weights[1],w1=weights[2],b1=weights[3])
    np.savez(a.output/'held-out.npz',rows=np.concatenate(rows))
    report={'architecture':[12,16,6],'features':8,'feature_base':[levels[0].shape[1],levels[0].shape[0]],
        'mips':len(levels),'feature_bytes':len(payload),'decoder_bytes':1240,'source_hash':digest.hex(),'held_out_queries':len(errors),
        'mae_channels':errors.mean(0).tolist(),'p95_abs_channels':np.quantile(errors,.95,axis=0).tolist(),
        'normal_angle_p95_deg':float(np.quantile(angles,.95)),'benchmark_performed':False}
    (a.output/'quality.json').write_text(json.dumps(report,indent=2)+'\n');print(report,flush=True)

if __name__=='__main__':main()

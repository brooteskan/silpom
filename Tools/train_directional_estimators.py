"""Full-wall neural directional fields/cap unions, frozen shared-material inputs.

Experimental lighting only. Never modifies the approved geometry or bundles.
Spatial UV blocks separate training/calibration/test receivers. Test labels are
constructed only after checkpoint selection. Four reach bands are decoded
together; nested visibility is enforced in the evaluator, not a penalty.
"""
import argparse, ctypes, hashlib, json, os
from pathlib import Path
os.environ.setdefault('OMP_NUM_THREADS', '4')
import numpy as np
import torch
from directional_occlusion import cosine_directions, query_directions, REACH_CAPS
from material_codec import HEADER
from relief_appearance import load_wall, queries
from train_shared_depth import descriptor, shared_inputs, render
from frozen_bundle import source_fingerprint
from level_one import FP,UP,U,F

INPUTS, HIDDEN = 20, 32

def bundle(path, source):
    raw=path.read_bytes(); h=HEADER.unpack_from(raw)
    if h[:8] != (b'SILPSHR4',2,1664,1536,11,6656,6144,575) or h[8]!=source:
        raise ValueError('Stale/incompatible shared feature bundle')
    weights=np.frombuffer(raw, '<f4',575,HEADER.size).copy(); offset=HEADER.size+575*4
    levels=[]; codes=[]; w,v=h[2:4]
    for _ in range(h[4]):
        n=w*v*12; a=np.frombuffer(raw[offset:offset+n],np.uint8).copy().reshape(v,w,12).astype(np.float32)/255
        levels.append(torch.from_numpy(a[:,:,:8])); codes.append(torch.from_numpy(a[:,:,8:])); offset+=n
        w,v=max(1,w//2),max(1,v//2)
    if offset!=len(raw):raise ValueError('Shared bundle length')
    return weights,levels,codes,hashlib.sha256(raw).hexdigest()

def partition(uv):
    cell=np.floor(np.clip(uv,0,.999999)*32).astype(np.int64)
    return ((cell[:,0]*73856093) ^ (cell[:,1]*19349663)) % 10

def dataset(name,count,seed,p,args,inputs,contract):
    cache=args.output/(name+'.npz')
    if cache.exists():
        z=np.load(cache)
        if str(z['contract'])!=contract:raise ValueError('Dataset contract changed: '+name)
        return {k:z[k] for k in ('x','y','directions','q','ids')}
    rng=np.random.default_rng(seed); qs=[]; ii=[]
    while sum(len(a) for a in qs)<count:
        q,ids,c=queries(p,count,seed+len(qs)*137)
        split=partition(q[:,:2]); keep=split<8 if name=='train' else split==8 if name=='calibration' else split==9
        qs.append(q[keep]); ii.append(ids[keep])
    q=np.concatenate(qs)[:count]; ids=np.concatenate(ii)[:count]
    # Include point receivers and moderate/distant footprints. Orientation is
    # attached to UV, and has no camera direction in the neural descriptor.
    minor=2**rng.uniform(0,10,count); minor[:count//3]=1
    major=minor*2**rng.uniform(0,3,count); major[:count//3]=1
    q[:,9:11]=np.column_stack((major,minor))
    angle=rng.uniform(-np.pi,np.pi,count);q[:,11:13]=np.column_stack((np.cos(angle),np.sin(angle)))
    lu=np.linalg.norm(p.du[ids],axis=1);lv=np.linalg.norm(p.dv[ids],axis=1)
    context=np.column_stack((np.minimum(12,np.log2(major))/13,np.minimum(12,np.log2(minor))/13,
        np.cos(2*angle),np.sin(2*angle),np.log2(lu)/16,np.log2(lv)/16,np.sum(p.du[ids]*p.dv[ids],1)/lu/lv)).astype(np.float32)
    d=descriptor(p,q,ids,context)
    with torch.no_grad():
        old=shared_inputs(inputs[1],inputs[2],inputs[0],q,context,d).numpy()
    x=np.column_stack((old[:,:8],old[:,11:17],old[:,18:20],context[:,:4])).astype(np.float32)
    canonical=cosine_directions(32).astype(np.float32)
    directions=np.concatenate((canonical,query_directions(32,seed+31).astype(np.float32)))
    y=np.empty((count,4,len(directions)),np.float32)
    dll=ctypes.CDLL(str((args.binary/'silpom_appearance_reference.dll').resolve()))
    dll.RenderDirectionalVisibility.argtypes=[FP,U,U,F,F,U,FP,U,FP,UP,U,U,FP]
    dll.RenderDirectionalVisibility.restype=ctypes.c_bool
    for band,reach in enumerate(REACH_CAPS):
        for start in range(0,count,256):
            sub=q[start:start+256]; n=len(sub); qq=np.repeat(sub,len(directions),axis=0)
            qq[:,5:8]=np.tile(directions,(n,1)); qq[:,8]=reach
            out=np.empty((len(qq),3),np.float32);faceids=np.ascontiguousarray(np.repeat(ids[start:start+n],len(directions)),np.uint32)
            if not dll.RenderDirectionalVisibility(p.height,*p.atlas,p.scale,p.reference,p.address,p.packed,len(p.packed),
                np.ascontiguousarray(qq,np.float32),faceids,len(qq),args.samples,out):raise ValueError('Teacher rejected query')
            known=(out[:,1]>.9999)&(out[:,2]==0)
            y[start:start+n,band]=np.where(known,out[:,0],-1).reshape(n,-1)
        print(name,'reach',reach,'complete',flush=True)
    # Independent reference quadrature can produce tiny nested differences.
    pairs=(y[:,1:]>=0)&(y[:,:-1]>=0)
    if np.any((np.diff(y,axis=1)>1e-5)&pairs):raise ValueError('Nonmonotone teacher')
    if (y>=0).mean()<.95:raise ValueError('Insufficient known teacher coverage')
    np.savez_compressed(cache,x=x,y=y,directions=directions,q=q,ids=ids,contract=contract)
    return dict(x=x,y=y,directions=directions,q=q,ids=ids)

def values(raw,kind,directions):
    if kind=='field':
        # Cumulative minimum makes each longer reach no more visible.
        return torch.cummin(torch.sigmoid(raw.reshape(-1,4,32)),dim=1).values
    k=int(kind);a=raw[:,:k*3].reshape(-1,k,3)
    a=torch.nn.functional.normalize(a,dim=-1,eps=1e-6)
    c=torch.sort(torch.sigmoid(raw[:,k*3:].reshape(-1,k,4)),dim=-1,descending=True).values
    margin=torch.einsum('nd,bkd->bnk',directions,a)[:,None]-c.transpose(1,2)[:,:,None,:]
    return torch.sigmoid(-32*margin.amax(dim=-1))

def fit(kind,train,cal,args,device):
    torch.manual_seed(19283+len(kind)); outputs=128 if kind=='field' else int(kind)*7
    model=torch.nn.Sequential(torch.nn.Linear(INPUTS,HIDDEN),torch.nn.ReLU(),torch.nn.Linear(HIDDEN,outputs)).to(device)
    # Centers start at different azimuths below the hemisphere: avoid an
    # identical-cap symmetry, while training only the final union visibility.
    if kind!='field':
        k=int(kind)
        with torch.no_grad():
            a=torch.tensor([[np.cos(i*2*np.pi/k),np.sin(i*2*np.pi/k),-.25] for i in range(k)],device=device,dtype=torch.float32)
            model[2].bias[:3*k]=a.flatten();model[2].weight[:3*k]*=.1
    optimizer=torch.optim.Adam(model.parameters(),lr=.002)
    tensors=[]
    for data in (train,cal):
        tensors.append((torch.from_numpy(data['x']).to(device),torch.from_numpy(data['y'][:,:,:32] if kind=='field' else data['y']).to(device),
            torch.from_numpy(data['directions']).to(device)))
    best=None;score=float('inf');selected=0
    for step in range(args.steps):
        x,y,d=tensors[0]; ids=torch.randint(len(x),(256,),device=device)
        v=values(model(x[ids]),kind,d);known=y[ids]>=0
        loss=torch.nn.functional.binary_cross_entropy(v[known].clamp(1e-6,1-1e-6),y[ids][known])
        optimizer.zero_grad(set_to_none=True);loss.backward();optimizer.step()
        if step%100==0 or step==args.steps-1:
            with torch.no_grad():
                x,y,d=tensors[1]; pred=values(model(x),kind,d); current=(pred-y).abs()[y>=0].mean().item()
            if current<score:score=current;selected=step;best={k:v.detach().cpu().clone() for k,v in model.state_dict().items()}
        if step%1000==0:print(kind,step,float(loss.detach()),score,flush=True)
    model.load_state_dict(best);model.cpu()
    torch.save(best,args.output/('head-'+kind+'.pt'))
    return model,dict(calibration_mae=score,selected_step=selected)

def export(models,path):
    lines=['// SPDX-License-Identifier: MIT','#pragma once', '// Generated by train_directional_estimators.py; experimental full-wall heads.']
    for kind,model in models.items():
        prefix='SilPomDirectional'+('Field' if kind=='field' else 'Caps'+kind)
        for suffix,a in (('W0',model[0].weight),('B0',model[0].bias),('W1',model[2].weight),('B1',model[2].bias)):
            flat=a.detach().numpy().ravel()
            lines.append('static const float '+prefix+suffix+'['+str(len(flat))+'] = {')
            for j in range(0,len(flat),8):lines.append('    '+', '.join(f'{float(v):.9e}f' for v in flat[j:j+8])+',')
            lines.append('};')
    path.write_text('\n'.join(lines)+'\n')

def main():
    cli=argparse.ArgumentParser(description=__doc__)
    for name in ('project','binary','output'):cli.add_argument(name,type=Path)
    cli.add_argument('--receivers',type=int,default=12288);cli.add_argument('--steps',type=int,default=10000);cli.add_argument('--samples',type=int,default=16)
    args=cli.parse_args();args.output.mkdir(parents=True,exist_ok=True);torch.set_num_threads(4)
    p,wall,mesh=load_wall(args.project,args.binary);source=source_fingerprint(wall,mesh)
    # The trained wall profile uses repeat; existing appearance training used
    # clamp. This independent visibility teacher must match the live profile.
    p.address=0
    inputs=bundle(args.project/'Assets/SilPOMLearned/wall-shared.l4shared',source)
    contract=json.dumps(dict(schema=1,source=source.hex(),shared=inputs[3],teacher=hashlib.sha256((args.binary/'silpom_appearance_reference.dll').read_bytes()).hexdigest(),
        source_code=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),samples=args.samples,scale=p.scale,address=p.address,reach=list(REACH_CAPS)),sort_keys=True)
    (args.output/'contract.json').write_text(contract)
    train=dataset('train',args.receivers,18831,p,args,inputs,contract)
    cal=dataset('calibration',2048,19871,p,args,inputs,contract)
    device='cuda' if torch.cuda.is_available() else 'cpu';print('Training on',device,flush=True)
    models={}; report={}
    for kind in ('field','4','8'):models[kind],report[kind]=fit(kind,train,cal,args,device)
    # No final test label participates in checkpoint selection.
    test=dataset('test',2048,47191,p,args,inputs,contract)
    for kind,model in models.items():
        with torch.no_grad():
            pred=values(model(torch.from_numpy(test['x'])),kind,torch.from_numpy(test['directions'])).numpy()
        target=test['y'][:,:,:32] if kind=='field' else test['y']
        known=target>=0;constant=train['y'][train['y']>=0].mean()
        report[kind]['test_mae']=float(np.abs(pred-target)[known].mean())
        report[kind]['test_constant_mae']=float(np.abs(target-constant)[known].mean())
        report[kind]['test_unknown']=int((~known).sum())
        if not np.isfinite(pred).all():raise ValueError('Nonfinite trained model')
    export(models,args.output/'DirectionalEstimatorWeights.azsli')
    report.update(contract=json.loads(contract),train_receivers=len(train['x']),calibration_receivers=len(cal['x']),test_receivers=len(test['x']),
        spatial_split='32x32 UV blocks, hash mod 10: 0-7 train, 8 calibration, 9 test',benchmark_performed=False,
        experimental=True,filter='16 stratified reference samples, camera-independent UV footprint context',
        reference='frozen affine local displacement direction; continued material; scene shadows remain external')
    (args.output/'quality.json').write_text(json.dumps(report,indent=2)+'\n');print(report,flush=True)

if __name__=='__main__':main()

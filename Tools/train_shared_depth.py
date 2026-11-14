"""Train the independent, default-off 4B shared visibility and 5B depth heads.

Existing 4A material bytes and 5A appearance bytes are frozen. Local detailed
heightfield tracing is offline only; no runtime confidence or recovery path.
"""
import argparse,ctypes,hashlib,json,struct
from pathlib import Path
import numpy as np
from PIL import Image
import torch
from relief_appearance import load_wall,queries,features,pack,network,HEADER
from material_codec import filtered
from level_one import FP,UP,U,F
from frozen_bundle import read_verified_bundle,source_fingerprint

def read_bundle(path,channels,kind,expected_source):
    raw,h=read_verified_bundle(path,kind,expected_source);offset=68+h[7]*4;levels=[];w,v=h[2:4]
    for _ in range(h[4]):
        size=w*v*channels
        levels.append(torch.from_numpy(np.frombuffer(raw[offset:offset+size],np.uint8).copy().reshape(v,w,channels).astype(np.float32)/255))
        offset+=size;w,v=max(1,w//2),max(1,v//2)
    assert offset==len(raw)
    return raw,h,np.frombuffer(raw[68:68+h[7]*4],'<f4').copy(),levels

def directional_codes(wall,levels):
    # Four cardinal height horizons. Material channels remain bit-identical.
    size=(1664,1536);height=np.asarray(Image.open(wall/'height.png').convert('F').resize(size,Image.Resampling.BOX),np.float32)/65535
    codes=[]
    for axis,sign in ((1,1),(1,-1),(0,1),(0,-1)):
        horizon=np.zeros_like(height)
        for distance in (1,2,4,8,16,32):
            index=np.clip(np.arange(height.shape[axis])+sign*distance,0,height.shape[axis]-1)
            slope=(np.take(height,index,axis=axis)-height)*size[1-axis]*.05/distance
            horizon=np.maximum(horizon,slope)
        codes.append(np.arctan(horizon)/(np.pi*.5))
    image=np.stack(codes,2);result=[]
    for material in levels:
        h,w=material.shape[:2]
        value=np.stack([np.asarray(Image.fromarray(image[...,c],'F').resize((w,h),Image.Resampling.BOX)) for c in range(4)],2)
        result.append(torch.from_numpy(np.round(np.clip(value,0,1)*255).astype(np.float32)/255))
    return result

def descriptor(p,q,ids,context):
    # Same affine center direction used by the offline local continuation.
    f=p.packed[ids];a=f[:,11:13]-f[:,9:11];b=f[:,13:15]-f[:,9:11];x=q[:,:2]-f[:,9:11]
    det=a[:,0]*b[:,1]-a[:,1]*b[:,0]
    u=(x[:,0]*b[:,1]-x[:,1]*b[:,0])/det;v=(a[:,0]*x[:,1]-a[:,1]*x[:,0])/det
    d=f[:,15:18]+u[:,None]*(f[:,18:21]-f[:,15:18])+v[:,None]*(f[:,21:24]-f[:,15:18])
    tangent=p.du[ids]/np.linalg.norm(p.du[ids],axis=1,keepdims=True);normal=p.packed[ids,24:27]
    bitangent=np.cross(normal,tangent);bitangent/=np.linalg.norm(bitangent,axis=1,keepdims=True)
    return np.column_stack((np.einsum('ni,ni->n',d,tangent),np.einsum('ni,ni->n',d,bitangent),np.einsum('ni,ni->n',d,normal))).astype(np.float32)

def render(p,binary,q,ids,samples,visibility):
    dll=ctypes.CDLL(str((binary/'silpom_appearance_reference.dll').resolve()))
    dll.RenderSharedDepth.argtypes=[FP,U,U,F,F,FP,U,FP,UP,U,U,ctypes.c_bool,FP];dll.RenderSharedDepth.restype=ctypes.c_bool
    out=np.empty((len(q),3),np.float32)
    assert dll.RenderSharedDepth(p.height,*p.atlas,p.scale,p.reference,p.packed,len(p.packed),q,ids,len(q),samples,visibility,out)
    assert np.isfinite(out).all()
    return out

def shared_inputs(levels,codes,w,q,context,d):
    qq=tuple(torch.from_numpy(v) for v in (q[:,:2],q[:,9],q[:,10],q[:,11:13],context[:,:4]))
    with torch.no_grad():
        z=filtered(levels,qq,True)*2-1
        x=torch.cat((z,qq[-1]),1);weights=torch.from_numpy(w)
        decoded=(torch.relu(x@weights[:192].reshape(16,12).T+weights[192:208])@weights[208:304].reshape(16,6)+weights[304:310]).clamp(0,1)
        aux=filtered(codes,qq,True)*2-1
        state=torch.cat((aux,decoded[:,3:6]*2-1,(decoded[:,:3]@torch.tensor([.2126,.7152,.0722]))[:,None]*2-1),1)
        return torch.cat((state,torch.from_numpy(q[:,5:8]),torch.from_numpy(context[:,4:7]),torch.from_numpy(d),torch.from_numpy(q[:,8:9]/.3),torch.from_numpy(q[:,:2]*2-1)),1)

def depth_inputs(levels,q,context,d):
    with torch.no_grad():
        return torch.cat((features(levels,torch.from_numpy(q)),torch.from_numpy(q[:,2:5]),torch.from_numpy(context),torch.from_numpy(d),torch.from_numpy(q[:,:2]*2-1)),1)

def boundary_edges(mesh_path):
    mesh=json.loads(mesh_path.read_bytes());faces=sorted(mesh['faces'],key=lambda f:f['stable_id'])
    # Match the runtime's geometric welding before finding material/exterior edges.
    vertices={k:tuple(round(float(x),5) for x in v) for k,v in mesh['vertices'].items()};edges={}
    for i,f in enumerate(faces):
        for e in range(3):
            key=tuple(sorted((vertices[str(f['vertices'][e])],vertices[str(f['vertices'][(e+1)%3])])));edges.setdefault(key,[]).append((i,e))
    keep=[i for i,f in enumerate(faces) if f['region']!=0 and f['profile']==1];local={j:i for i,j in enumerate(keep)};result=[]
    for neighbors in edges.values():
        for i,e in neighbors:
            if i in local and (len(neighbors)!=2 or any(j not in local or faces[j]['material_id']!=faces[i]['material_id'] for j,_ in neighbors)):
                result.append((local[i],e))
    if not result:raise ValueError('No boundary fixtures found')
    return np.asarray(result,np.int64)

def make_queries(p,count,seed,edges):
    q,ids,c=queries(p,count,seed);rng=np.random.default_rng(seed+7)
    q[:,10]=2**rng.uniform(0,12.8,count);q[:,9]=q[:,10]*2**rng.uniform(0,3,count)
    c[:,0]=np.minimum(12,np.log2(q[:,9]))/13;c[:,1]=np.minimum(12,np.log2(q[:,10]))/13
    boundary=np.zeros(count,bool);n=count//5;chosen=edges[rng.integers(len(edges),size=n)];ids[:n]=chosen[:,0]
    t=rng.uniform(.05,.95,n);epsilon=10**rng.uniform(-4,-1.5,n);uv=p.uv[ids[:n]]
    q[:n,:2]=(uv[np.arange(n),chosen[:,1]]*(1-t[:,None])+uv[np.arange(n),(chosen[:,1]+1)%3]*t[:,None])*(1-epsilon[:,None])+uv[np.arange(n),(chosen[:,1]+2)%3]*epsilon[:,None]
    boundary[:n]=True
    # Boundary reassignment changes the physical metric, not the footprint.
    u,v=p.du[ids],p.dv[ids];lu,lv=np.linalg.norm(u,axis=1),np.linalg.norm(v,axis=1)
    c[:,4:7]=np.column_stack((np.log2(lu)/16,np.log2(lv)/16,np.einsum('ni,ni->n',u,v)/lu/lv))
    return q,ids,c,boundary

def metrics(pred,target,q,boundary,p,ids,shared):
    error=(pred-target).ravel();absolute=abs(error)
    def summarize(mask):
        if not mask.any():return {'count':0}
        e=error[mask];a=abs(e)
        return {'count':int(mask.sum()),'mse':float(np.mean(e*e)),'mae':float(a.mean()),'p95_absolute_error':float(np.quantile(a,.95))}
    result=summarize(np.ones(len(q),bool))
    result['subsets']={name:summarize(mask) for name,mask in {
        'grazing_view':q[:,4]<.5,'grazing_light':q[:,7]<.2,'large_footprint':q[:,10]>=512,
        'long_finite_reach':q[:,8]>=.3,'unbounded_reach':q[:,8]==0,'material_or_exterior_boundary':boundary}.items()}
    if not shared:
        d=descriptor(p,q,ids,None);height_delta=p.scale*error;ray_delta=height_delta*d[:,2]/q[:,4]
        base=np.abs(p.scale*(target.ravel()-p.reference)*d[:,2]/q[:,4])
        u,v,N=p.du[ids],p.dv[ids],p.packed[ids,24:27];T=u/np.linalg.norm(u,axis=1,keepdims=True);B=np.cross(N,T);B/=np.linalg.norm(B,axis=1,keepdims=True)
        V=T*q[:,2:3]+B*q[:,3:4]+N*q[:,4:5];D=T*d[:,:1]+B*d[:,1:2]+N*d[:,2:3]
        delta=V*ray_delta[:,None]-D*height_delta[:,None];cross=np.cross(v,N);det=np.einsum('ni,ni->n',u,cross)
        uv_delta=np.column_stack((np.einsum('ni,ni->n',delta,cross)/det,np.einsum('ni,ni->n',u,np.cross(delta,N))/det))
        uv_error=np.linalg.norm(uv_delta*np.array([6656,6144]),axis=1)
        result.update(height_mae_metres=float(absolute.mean()*p.scale),ray_depth_mae_metres=float(abs(ray_delta).mean()),
            ray_depth_p95_metres=float(np.quantile(abs(ray_delta),.95)),authored_plane_ray_depth_mae_metres=float(base.mean()),
            resolved_uv_mae_source_texels=float(uv_error.mean()),resolved_uv_p95_source_texels=float(np.quantile(uv_error,.95)))
    else:result['unit_visibility_mse']=float(np.mean((1-target)**2))
    return result

def prepare_set(label,count,seed,p,a,folder,sourcehash,reference_hash,edges,shared,levels,codes,w):
    cache=folder/(label+'.npz')
    if cache.exists():
        data=np.load(cache);q,ids,c,target,boundary=[data[k] for k in ('q','ids','context','target','boundary')]
        if str(data['reference_hash'])!=reference_hash or int(data['seed'])!=seed or len(q)!=count:
            raise ValueError('Stale reference cache; use a new output directory')
    else:
        q,ids,c,boundary=make_queries(p,count,seed,edges)
        print('Rendering '+('shared ' if shared else 'depth ')+label,flush=True);target=render(p,a.binary,q,ids,a.samples,shared)
        np.savez(cache,q=q,ids=ids,context=c,target=target,boundary=boundary,reference_hash=reference_hash,seed=seed)
    valid=(target[:,1]>.99)&(target[:,2]==0)
    if valid.sum()<=len(q)*.8:raise ValueError('Insufficient uncapped reference coverage')
    q,ids,c,target,boundary=[v[valid] for v in (q,ids,c,target,boundary)]
    d=descriptor(p,q,ids,c);x=shared_inputs(levels,codes,w,q,c,d) if shared else depth_inputs(levels,q,c,d)
    print(label+': '+str(len(q))+' usable',flush=True)
    return x,torch.from_numpy(target[:,:1]),q,ids,boundary

def main():
    cli=argparse.ArgumentParser(description=__doc__);cli.add_argument('project',type=Path);cli.add_argument('binary',type=Path);cli.add_argument('output',type=Path)
    cli.add_argument('--queries',type=int,default=12288);cli.add_argument('--steps',type=int,default=2500);cli.add_argument('--samples',type=int,default=16)
    cli.add_argument('--kind',choices=['shared','depth','both'],default='both');a=cli.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    torch.manual_seed(41851);p,wall,mesh=load_wall(a.project,a.binary)
    sourcehash=source_fingerprint(wall,mesh);materialhash=source_fingerprint(wall);edges=boundary_edges(mesh)
    teacherhash=hashlib.sha256((a.binary/'silpom_appearance_reference.dll').read_bytes()).hexdigest()
    reference_hash=hashlib.sha256((sourcehash.hex()+teacherhash+str(a.samples)+'boundary-query-contract-v1').encode()).hexdigest()
    for kind in ('shared','depth'):
        if a.kind not in ('both',kind):continue
        shared=kind=='shared';folder=a.output/kind;folder.mkdir(exist_ok=True)
        raw,h,w,levels=read_bundle(a.project/('Assets/SilPOMLearned/wall.l4mat' if shared else 'Assets/SilPOMLearned/wall-appearance.l5app'),8,
            'material' if shared else 'appearance',materialhash if shared else sourcehash)
        frozen_hash=hashlib.sha256(raw).hexdigest()
        codes=directional_codes(wall,levels) if shared else None
        sets=[];participation=np.zeros(h[4],int)
        for label,count,seed in (('train',a.queries,18551),('validation',4096,19391)):
            data=prepare_set(label,count,seed,p,a,folder,sourcehash,reference_hash,edges,shared,levels,codes,w)
            x,target,q,ids,boundary=data;sets.append(data)
            if label=='train':
                lod=np.clip(np.log2(q[:,10])-(2 if shared else 3),0,h[4]-1)
                participation=np.array([int(((np.floor(lod)==i)|(np.ceil(lod)==i)).sum()) for i in range(h[4])]);assert (participation>0).all()
        model=torch.nn.Sequential(torch.nn.Linear(20,12),torch.nn.ReLU(),torch.nn.Linear(12,1)) if shared else network(23,1)
        optimizer=torch.optim.Adam(model.parameters(),lr=.002);g=torch.Generator().manual_seed(91811)
        x,t=sets[0][:2];best=None;bestloss=float('inf');best_step=None
        for step in range(a.steps):
            ids=torch.randint(len(x),(512,),generator=g);prediction=torch.sigmoid(model(x[ids]).clamp(-16,16))
            loss=(prediction-t[ids]).square().mean();optimizer.zero_grad(set_to_none=True);loss.backward();optimizer.step()
            if step%100==0 or step==a.steps-1:
                with torch.no_grad():held=(torch.sigmoid(model(sets[1][0]).clamp(-16,16))-sets[1][1]).square().mean().item()
                if held<bestloss:bestloss=held;best_step=step;best={k:v.detach().clone() for k,v in model.state_dict().items()}
            if step%500==0:print(kind,step,float(loss.detach()),bestloss,flush=True)
        model.load_state_dict(best)
        for parameter in model.parameters():parameter.requires_grad_(False)
        baseline=(sets[1][1]-sets[0][1].mean()).square().mean().item();assert bestloss<baseline,'Head did not beat a validation constant'
        # Checkpoint selection is finished. Only now construct/read the final
        # test partition; its labels never enter an optimizer or selector.
        test=prepare_set('test',4096,74053,p,a,folder,sourcehash,reference_hash,edges,shared,levels,codes,w)
        final_metrics={}
        for label,data in (('validation',sets[1]),('test',test)):
            xx,tt,qq,ii,bb=data
            with torch.no_grad():pred=torch.sigmoid(model(xx).clamp(-16,16)).numpy()
            final_metrics[label]=metrics(pred,tt.numpy(),qq,bb,p,ii,shared)
        weights=np.concatenate((w,pack(model))).astype('<f4');expected=575 if shared else 1158;assert len(weights)==expected
        payload=b''.join((torch.cat((v,codes[i]),2) if shared else v).mul(255).round().byte().numpy().tobytes() for i,v in enumerate(levels))
        header=HEADER.pack(b'SILPSHR4' if shared else b'SILPDEP5',2,*h[2:7],len(weights),sourcehash)
        output=header+weights.tobytes()+payload;name='wall-shared.l4shared' if shared else 'wall-depth.l5depth'
        (folder/name).write_bytes(output)
        report=dict(kind=kind,train_queries=len(sets[0][0]),validation=final_metrics['validation'],test=final_metrics['test'],samples_per_pixel=a.samples,steps=a.steps,
            selection_metric='validation MSE only',selected_step=best_step,validation_constant_mse=baseline,test_evaluated_after_freeze=True,
            partition_seeds={'train':18551,'validation':19391,'test':74053},frozen_input_bundle_sha256=frozen_hash,
            frozen_input_source_sha256=h[8].hex(),reference_renderer_sha256=teacherhash,reference_contract_sha256=reference_hash,
            training_mip_participation=participation.tolist(),frozen_original_features_and_decoder=True,coverage_predicted=False,
            bytes=len(output),sha256=hashlib.sha256(output).hexdigest(),source_sha256=sourcehash.hex(),benchmark_performed=False,
            reference='filtered local detailed heightfield; source UV metric and affine center D frozen per query')
        (folder/'quality.json').write_text(json.dumps(report,indent=2)+'\n');print(report,flush=True)

if __name__=='__main__':main()

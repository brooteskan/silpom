"""Train coarse appearance mips using a continued local relief reference.

Large pixel footprints cross source triangles. Coverage belongs to the
authored proxy, so coarse material training continues a local heightfield
instead of dropping these samples at an incidental triangulation boundary.
Keep the validated decoder and fine feature mips fixed during this step.
"""
import argparse,hashlib,json,struct
from pathlib import Path
import numpy as np
from PIL import Image
import torch
from relief_appearance import load_wall,queries,reference,network,run_models,shade,HEADER

def restore(model,w):
    inputs=model[0].in_features;outputs=model[2].out_features;bias=inputs*16;second=bias+16;last=second+16*outputs
    with torch.no_grad():
        model[0].weight.copy_(torch.from_numpy(w[:bias].copy().reshape(16,inputs)))
        model[0].bias.copy_(torch.from_numpy(w[bias:second].copy()))
        model[2].weight.copy_(torch.from_numpy(w[second:last].copy().reshape(16,outputs).T.copy()))
        model[2].bias.copy_(torch.from_numpy(w[last:last+outputs].copy()))
    for p in model.parameters():p.requires_grad_(False)

def material_maps(wall):
    color=np.asarray(Image.open(wall/'basecolor.png'),np.float32)/255
    color=np.where(color<=.04045,color/12.92,((color+.055)/1.055)**2.4)
    maps=np.empty((*color.shape[:2],6),np.float32);maps[...,:3]=color;del color
    normal=np.asarray(Image.open(wall/'normal.png'));maps[...,3]=normal[...,1]/255;maps[...,4]=normal[...,0]/255;del normal
    rough=np.asarray(Image.open(wall/'roughness.png'));maps[...,5]=rough[...,0]/255
    return maps

def render_loss(levels,surface,light,q,context,target):
    s,gain=run_models(levels,surface,light,q,context)
    diffuse,spec,n,rough=shade(s,q[:,2:5],q[:,5:8],target[:,8:11])
    prediction=torch.cat((diffuse*gain[:,:1],spec[:,None]*gain[:,1:2]),1)
    loss=(torch.log1p(prediction*8)-torch.log1p(target[:,4:8]*8)).square().mean()
    loss+=.015*(n-torch.nn.functional.normalize(target[:,:3],dim=1)).square().mean()+.01*(rough-target[:,3]).square().mean()
    return loss,prediction

def finalize(fine_bundle,output):
    raw=fine_bundle.read_bytes();candidate=(output/'wall.l5app').read_bytes()
    report=json.loads((output/'quality.json').read_text());surface=network(18,3);light=network(22,2)
    weights=np.frombuffer(raw[68:3096],'<f4');restore(surface,weights[:355]);restore(light,weights[355:])
    assert raw[12:3096]==candidate[12:3096]
    def decode(data):
        result=[];offset=3096;w,h=832,768
        for i in range(10):
            count=w*h*8;result.append(torch.from_numpy(np.frombuffer(data[offset:offset+count],np.uint8).copy().reshape(h,w,8).astype(np.float32)/255))
            offset+=count;w,h=max(1,w//2),max(1,h//2)
        assert offset==len(data)
        return result
    original,final=decode(raw),decode(candidate)
    for i in range(7):assert torch.equal(original[i],final[i]),'Fine feature mip changed'
    cache=np.load(fine_bundle.parent/'held-out.npz');reference=cache['reference'];valid=(reference[:,11]>.5)&(reference[:,12]==0)
    q,c,t=[torch.from_numpy(a[valid]) for a in (cache['q'],cache['context'],reference)]
    errors=[];percentiles=[]
    with torch.no_grad():
        for levels in (original,final):
            rows=[]
            for start in range(0,len(q),512):
                _,pred=render_loss(levels,surface,light,q[start:start+512],c[start:start+512],t[start:start+512])
                rows.append((pred-t[start:start+512,4:8]).abs().numpy())
            joined=np.concatenate(rows);errors.append(joined.mean(0));percentiles.append(np.quantile(joined,.95,axis=0))
    assert errors[1].sum()<=errors[0].sum()*1.01+1e-6,'Fine held-out rendering regressed'
    cache=np.load(fine_bundle.parent/'train.npz');valid=(cache['reference'][:,11]>.5)&(cache['reference'][:,12]==0)
    lod=np.clip(np.log2(cache['q'][valid,10])-3,0,9)
    fine=np.array([int(((np.floor(lod)==i)|(np.ceil(lod)==i)).sum()) for i in range(10)])
    combined=fine+np.array(report['coarse_training_mip_participation']);assert (combined>0).all()
    # Version 1 is a provisional fine fit. Only a complete, validated tail is
    # promoted to the version 2 runtime format.
    final_bytes=candidate[:8]+struct.pack('<I',2)+candidate[12:]
    report.update(fine_holdout_mae_before=errors[0].tolist(),fine_holdout_mae_after=errors[1].tolist(),
        fine_holdout_p95_abs_after=percentiles[1].tolist(),
        fine_training_mip_participation=fine.tolist(),combined_mip_participation=combined.tolist(),all_mips_trained=True,
        final_bundle_version=2,bundle_sha256=hashlib.sha256(final_bytes).hexdigest())
    (output/'wall.l5app').write_bytes(final_bytes);(output/'quality.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Finalized all feature mips; fine held-out MAE '+str(errors[1].tolist()),flush=True)

def main():
    cli=argparse.ArgumentParser(description=__doc__);cli.add_argument('project',type=Path);cli.add_argument('binary',type=Path)
    cli.add_argument('bundle',type=Path);cli.add_argument('output',type=Path);cli.add_argument('--steps',type=int,default=1500)
    cli.add_argument('--finalize-only',action='store_true',help='Validate/promote an existing fitted tail without rerendering or refitting')
    a=cli.parse_args();a.output.mkdir(parents=True,exist_ok=True);torch.manual_seed(81517)
    if a.finalize_only:finalize(a.bundle,a.output);return
    raw=a.bundle.read_bytes();header=HEADER.unpack_from(raw);assert header[:2]==(b'SILPAPP5',1)
    weights=np.frombuffer(raw[68:68+757*4],'<f4');surface=network(18,3);light=network(22,2)
    restore(surface,weights[:355]);restore(light,weights[355:])
    levels=[];offset=68+757*4;w,h=832,768
    for i in range(10):
        count=w*h*8;levels.append(torch.from_numpy(np.frombuffer(raw[offset:offset+count],np.uint8).copy().reshape(h,w,8).astype(np.float32)/255))
        offset+=count;w,h=max(1,w//2),max(1,h//2)
    assert offset==len(raw)
    parameters=[torch.nn.Parameter(t.clone()) for t in levels[7:]];original=[p.detach().clone() for p in parameters]
    def quantized():return levels[:7]+[p+(p.clamp(0,1).mul(255).round().div(255)-p).detach() for p in parameters]
    p,wall,mesh=load_wall(a.project,a.binary);maps=material_maps(wall)
    # Preserve each face's physical UV metric/frame; continue its local
    # material on a flat patch large enough for the complete pixel footprint.
    packed=p.packed.copy();old=packed[:,9:11].copy();new=np.array([-16,-16],np.float32)
    packed[:,:3]+=packed[:,3:6]*(new[0]-old[:,0,None])+packed[:,6:9]*(new[1]-old[:,1,None])
    packed[:,9:15]=[-16,-16,32,-16,-16,32]
    packed[:,15:24]=np.tile(packed[:,24:27],(1,3));p.packed=np.ascontiguousarray(packed)
    sets=[];participation=np.zeros(10,int)
    for label,count,seed in [('coarse-train',2048,17719),('coarse-held-out',1024,23819)]:
        q,ids,context=queries(p,count,seed);rng=np.random.default_rng(seed+1)
        q[:,10]=2**rng.uniform(9.5,12.7,count);q[:,9]=q[:,10]*2**rng.uniform(0,3,count)
        context[:,0]=np.minimum(12,np.log2(q[:,9]))/13;context[:,1]=np.minimum(12,np.log2(q[:,10]))/13
        print('Rendering '+label,flush=True);data=reference(p,a.binary,maps,q,ids,64)
        valid=(data[:,11]>.99)&(data[:,12]==0);assert valid.sum()>count*.8
        print(label+' usable '+str(int(valid.sum()))+'/'+str(count),flush=True)
        np.savez(a.output/(label+'.npz'),q=q,context=context,reference=data,valid=valid)
        if label=='coarse-train':
            lod=np.clip(np.log2(q[valid,10])-3,0,9)
            participation=np.array([int(((np.floor(lod)==i)|(np.ceil(lod)==i)).sum()) for i in range(10)])
        sets.append(tuple(torch.from_numpy(v[valid]) for v in (q,context,data)))
    del maps
    q,c,t=sets[1]
    with torch.no_grad():before,pred=render_loss(levels,surface,light,q,c,t);beforeMae=(pred-t[:,4:8]).abs().mean(0).tolist()
    optimizer=torch.optim.Adam(parameters,lr=.007);g=torch.Generator().manual_seed(11613)
    for step in range(a.steps):
        q,c,t=sets[0];ids=torch.randint(len(q),(256,),generator=g)
        loss,_=render_loss(quantized(),surface,light,q[ids],c[ids],t[ids])
        loss+=.00005*sum((p-o).square().mean() for p,o in zip(parameters,original))
        optimizer.zero_grad(set_to_none=True);loss.backward();optimizer.step()
        with torch.no_grad():
            for v in parameters:v.clamp_(0,1)
        if step%300==0:print(step,float(loss.detach()),flush=True)
    final=quantized();q,c,t=sets[1]
    with torch.no_grad():after,pred=render_loss(final,surface,light,q,c,t);afterMae=(pred-t[:,4:8]).abs().mean(0).tolist()
    assert float(after)<=float(before),'Coarse-tail held-out rendering regressed'
    payload=b''.join(v.detach().clamp(0,1).mul(255).round().byte().numpy().tobytes() for v in final)
    output=raw[:68+757*4]+payload;assert len(output)==len(raw)
    (a.output/'wall.l5app').write_bytes(output)
    report={'coarse_training_queries':len(sets[0][0]),'coarse_held_out_queries':len(q),'steps':a.steps,'samples_per_pixel':64,
        'feature_mips_optimized':[7,8,9],'other_feature_mips_and_decoder_frozen':True,'coarse_training_mip_participation':participation.tolist(),
        'held_out_loss_before':float(before),'held_out_loss_after':float(after),'mae_before':beforeMae,'mae_after':afterMae,
        'bundle_sha256':hashlib.sha256(output).hexdigest(),'benchmark_performed':False,
        'reference':'continued local heightfield with source-face UV metric; authored coverage remains runtime proxy geometry'}
    (a.output/'quality.json').write_text(json.dumps(report,indent=2)+'\n');print(report,flush=True)
    finalize(a.bundle,a.output)

if __name__=='__main__':main()

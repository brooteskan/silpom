"""Independent moving-light curves and diffuse-accessibility error after freeze."""
import argparse,ctypes,hashlib,json,math
from pathlib import Path
import numpy as np
import torch
from directional_occlusion import cosine_directions
from train_directional_estimators import bundle,values
from train_angular_edges import logits
from train_shared_depth import descriptor,shared_inputs
from relief_appearance import load_wall
from level_one import FP,UP,U,F
from frozen_bundle import source_fingerprint

def network(outputs,path):
    model=torch.nn.Sequential(torch.nn.Linear(20,32),torch.nn.ReLU(),torch.nn.Linear(32,outputs))
    model.load_state_dict(torch.load(path,weights_only=True));return model

def dense_accessibility(p,dll,q,ids,x,old,new):
    """Separate model error from angular quadrature error, without refitting."""
    # Fixed low/middle/high height strata, each at both footprint sizes.
    chosen=np.array([0,1,4,5,10,11]);q=q[chosen].copy();ids=ids[chosen];x=x[chosen]
    counts=(32,4096,8192);teacher=[];models={3:[],5:[]}
    for count in counts:
        directions=cosine_directions(count).astype(np.float32)
        qq=np.repeat(q,count,axis=0);qq[:,5:8]=np.tile(directions,(len(q),1));qq[:,8]=0
        ii=np.ascontiguousarray(np.repeat(ids,count),np.uint32);out=[]
        for start in range(0,len(qq),8192):
            batch=np.ascontiguousarray(qq[start:start+8192],np.float32);result=np.empty((len(batch),3),np.float32)
            assert dll.RenderDirectionalVisibility(p.height,*p.atlas,p.scale,p.reference,0,p.packed,len(p.packed),
                batch,np.ascontiguousarray(ii[start:start+len(batch)]),len(batch),16,result)
            assert np.all((result[:,1]>.9999)&(result[:,2]==0)), 'Unknown dense teacher labels'
            out.append(result[:,0])
        teacher.append(np.concatenate(out).reshape(len(q),count).mean(1))
        with torch.no_grad():
            d=torch.from_numpy(directions)
            models[3].append(values(old(x),'4',d)[:,3].mean(1).numpy())
            models[5].append(torch.sigmoid(logits(new(x),d))[:,3].mean(1).numpy())
        print('Dense ambient directions',count,'receivers',len(q),flush=True)
    reference=teacher[-1]
    rows=[]
    for i in range(len(q)):
        row=dict(uv=q[i,:2].tolist(),footprint=float(q[i,9]),teacher_32=float(teacher[0][i]),
            teacher_4096=float(teacher[1][i]),teacher_8192=float(reference[i]))
        for mode in (3,5):
            row['model_'+str(mode)+'_32']=float(models[mode][0][i])
            row['model_'+str(mode)+'_8192']=float(models[mode][-1][i])
        rows.append(row)
    return dict(direction_counts=list(counts),receiver_footprints=len(q),rows=rows,
        support='relief-top exit, 16 spatial footprint samples, cosine-weighted hemisphere',
        selection='fixed low/middle/high strata from the already frozen moving-light fixture; no fitting',
        reference='8192-direction numerical teacher estimate; 4096 vs 8192 is a convergence diagnostic, not an error bound',
        teacher_32_quadrature_mae=float(np.abs(teacher[0]-reference).mean()),
        teacher_4096_to_8192_max=float(np.abs(teacher[1]-reference).max()),
        old_runtime_to_dense_mae=float(np.abs(models[3][0]-reference).mean()),
        new_runtime_to_dense_mae=float(np.abs(models[5][0]-reference).mean()),
        new_dense_representation_mae=float(np.abs(models[5][-1]-reference).mean()),
        new_32_quadrature_mae=float(np.abs(models[5][0]-models[5][-1]).mean()))

def main():
    cli=argparse.ArgumentParser(description=__doc__)
    for name in ('project','binary','labels','model','output'):cli.add_argument(name,type=Path)
    a=cli.parse_args();a.output.mkdir(parents=True,exist_ok=True);torch.set_num_threads(4)
    test=np.load(a.labels/'test.npz');old=network(28,a.labels/'head-4.pt');new=network(32,a.model/'head-angular-edges.pt')
    canonical=torch.from_numpy(cosine_directions(32).astype(np.float32));truth=test['y'][:,3,:32].mean(1)
    with torch.no_grad():
        x=torch.from_numpy(test['x']);previous=torch.sigmoid(logits(new(x),canonical))[:,3].mean(1).numpy()
        baseline=values(old(x),'4',canonical)[:,3].mean(1).numpy()
    result=dict(ambient_test_receivers=len(x),ambient_quadrature=32,
        ambient_target='same 32 cosine directions, relief-top exit; isolates representation error from quadrature error',
        ambient_old_mae=float(np.abs(baseline-truth).mean()),ambient_new_mae=float(np.abs(previous-truth).mean()))
    p,wall,mesh=load_wall(a.project,a.binary);p.address=0
    inputs=bundle(a.project/'Assets/SilPOMLearned/wall-shared.l4shared',source_fingerprint(wall,mesh))
    candidates=np.flatnonzero(test['q'][:,9]==1);height=p.sample(test['q'][candidates,:2]);selected=[]
    for quantile in (.05,.25,.45,.65,.85,.95):
        for j in np.argsort(abs(height-np.quantile(height,quantile))):
            i=int(candidates[j])
            if i not in selected:selected.append(i);break
    q=np.repeat(test['q'][selected],2,axis=0).copy();ids=np.repeat(test['ids'][selected],2)
    q[:,9:11]=np.tile([[1,1],[16,16]],(6,1));q[:,11]=1;q[:,12]=0
    lu=np.linalg.norm(p.du[ids],axis=1);lv=np.linalg.norm(p.dv[ids],axis=1)
    context=np.column_stack((np.log2(q[:,9])/13,np.log2(q[:,10])/13,np.ones(len(q)),np.zeros(len(q)),
        np.log2(lu)/16,np.log2(lv)/16,np.sum(p.du[ids]*p.dv[ids],1)/lu/lv)).astype(np.float32)
    with torch.no_grad():
        shared=shared_inputs(inputs[1],inputs[2],inputs[0],q,context,descriptor(p,q,ids,context)).numpy()
    xx=torch.from_numpy(np.column_stack((shared[:,:8],shared[:,11:17],shared[:,18:20],context[:,:4])).astype(np.float32))
    azimuth=np.radians(np.linspace(0,360,361));directions=[]
    for elevation in (8,25):
        z=math.sin(math.radians(elevation));r=math.cos(math.radians(elevation))
        directions.extend(np.column_stack((r*np.cos(azimuth),r*np.sin(azimuth),np.full(len(azimuth),z))))
    directions=np.asarray(directions,np.float32);tt=torch.from_numpy(directions)
    with torch.no_grad():
        old_pred=values(old(xx),'4',tt).numpy();new_pred=torch.sigmoid(logits(new(xx),tt)).numpy()
    dll=ctypes.CDLL(str((a.binary/'silpom_appearance_reference.dll').resolve()))
    dll.RenderDirectionalVisibility.argtypes=[FP,U,U,F,F,U,FP,U,FP,UP,U,U,FP];dll.RenderDirectionalVisibility.restype=ctypes.c_bool
    labels=[]
    for band in (2,3):
        qq=np.repeat(q,len(directions),axis=0);qq[:,5:8]=np.tile(directions,(len(q),1));qq[:,8]=.3 if band==2 else 0
        ii=np.ascontiguousarray(np.repeat(ids,len(directions)),np.uint32);output=np.empty((len(qq),3),np.float32)
        assert dll.RenderDirectionalVisibility(p.height,*p.atlas,p.scale,p.reference,0,p.packed,len(p.packed),
            np.ascontiguousarray(qq,np.float32),ii,len(qq),16,output)
        labels.append(np.where((output[:,1]>.9999)&(output[:,2]==0),output[:,0],-1).reshape(len(q),-1))
    teacher=np.stack(labels,axis=1);old_curve=old_pred[:,[2,3]];new_curve=new_pred[:,[2,3]];known=teacher>=0
    result.update(moving_receiver_footprints=len(q),moving_angles=len(directions),moving_reaches=[.3,0],
        moving_old_mae=float(np.abs(old_curve-teacher)[known].mean()),moving_new_mae=float(np.abs(new_curve-teacher)[known].mean()),
        moving_unknown_labels=int((~known).sum()),teacher='continuous FP64 bilinear cells, 16 footprint samples, repeat',
        selection='fixed height-stratified held-out receivers; never used to select weights')
    np.savez_compressed(a.output/'moving-light.npz',uv=q[:,:2],footprint=q[:,9],directions=directions,teacher=teacher,old=old_curve,new=new_curve)
    result['dense_ambient']=dense_accessibility(p,dll,q,ids,xx,old,new)
    result['provenance']=dict(shared_feature_generation=inputs[3],source_fingerprint=source_fingerprint(wall,mesh).hex(),
        old_checkpoint_sha256=hashlib.sha256((a.labels/'head-4.pt').read_bytes()).hexdigest(),
        new_checkpoint_sha256=hashlib.sha256((a.model/'head-angular-edges.pt').read_bytes()).hexdigest(),
        teacher_binary_sha256=hashlib.sha256((a.binary/'silpom_appearance_reference.dll').read_bytes()).hexdigest())
    (a.output/'quality.json').write_text(json.dumps(result,indent=2)+'\n');print(result,flush=True)

if __name__=='__main__':main()

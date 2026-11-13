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
    (a.output/'quality.json').write_text(json.dumps(result,indent=2)+'\n');print(result,flush=True)

if __name__=='__main__':main()

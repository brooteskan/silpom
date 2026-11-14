"""Retrain neural-only visibility for the shared physical-reach lighting contract.

Historical hybrid coefficients and schedules are deliberately kept separately.
Train/calibration/test partitions use independent seeds. Calibration selects only
the binary cutoff; a fixed training schedule never selects a test checkpoint.
"""
import argparse,hashlib,json
from pathlib import Path
import numpy as np
from PIL import Image
from level_one import Evaluator,load_profiles
from level_three import Native,requests,fit,shader,predict
from evaluate_visibility_only import classification,threshold

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    cli=argparse.ArgumentParser(description=__doc__)
    cli.add_argument('manifest',type=Path);cli.add_argument('binary_dir',type=Path);cli.add_argument('output',type=Path)
    cli.add_argument('--training-rays',type=int,default=100000);cli.add_argument('--rays',type=int,default=20000)
    cli.add_argument('--steps',type=int,default=6500);cli.add_argument('--reuse',action='store_true')
    a=cli.parse_args();root=Path(__file__).resolve().parents[1];project=root.parents[1]
    if min(a.training_rays,a.rays,a.steps)<1:cli.error('Counts must be positive')
    if a.output.resolve().is_relative_to(root/'Assets'):cli.error('Datasets belong outside shipping assets')
    a.output.mkdir(parents=True,exist_ok=True)
    sources=['Tools/train_physical_visibility.py','Tools/level_three.py','Tools/level_one.py',
        'Tools/evaluate_visibility_only.py','Tools/visibility_evaluator.cpp','Tools/VisibilityCpu.h',
        'Tools/visibility_reference.h','Assets/Shaders/SilPOM/PhysicalShadowReach.azsli',
        'Assets/Shaders/SilPOM/VisibilityInputs.azsli','Assets/Shaders/SilPOM/NeuralVisibility.azsli']
    contract={p:digest(root/p) for p in sources}
    contract['native_evaluator_sha256']=digest(a.binary_dir/'silpom_visibility.dll')
    mesh=a.manifest.parent/json.loads(a.manifest.read_bytes())['sidecar']
    report={'contract':'v2: zero = relief-top exit; positive = min(cap, top exit); hRate <= 0 = shadow',
        'relief_depth_multiplier':.5,'architecture':[10,12,1],'training_steps':a.steps,
        'source_sha256':contract,'benchmark_performed':False,
        'baseline_comparison':'Historical schedule reach differs; baseline counts are diagnostic, not matched-target quality.',
        'partition_seeds':{'train':39913,'calibration':65239,'test':98239},'profiles':{}}
    native=Native(a.binary_dir);geometry=Evaluator(a.binary_dir);models={};cutoffs=[]
    paths=['IrishFieldstoneTowerWelded/wall','IrishFieldstoneTowerOpenShaft/sculpted-stair-silpom',
        'IrishFieldstoneTowerOpenShaft/welded-doorway/trim-blocks','IrishFieldstoneTowerOpenShaft/welded-doorway/reveal']
    for ident,name,p,_,_ in load_profiles(a.manifest,geometry):
        name=name.removeprefix('OpenShaft');model=ident+3
        path=project/'Assets/SilPOMPlanarAB'/paths[ident-1]/'height.png'
        p.height=np.ascontiguousarray(np.asarray(Image.open(path),np.float32)/65535);p.atlas=p.height.shape[::-1];p.scale*=.5
        folder=a.output/name;folder.mkdir(exist_ok=True)
        metadata={'sources':contract,'height':digest(path),'manifest':digest(a.manifest),
            'geometry':digest(mesh),'packed_faces':hashlib.sha256(p.packed.tobytes()).hexdigest(),'scale':p.scale,
            'reference':p.reference,'training_rays':a.training_rays,'rays':a.rays,'steps':a.steps}
        meta=folder/'contract.json'
        reuse=a.reuse and meta.exists() and json.loads(meta.read_text())==metadata
        if a.reuse and meta.exists() and not reuse:raise ValueError('Cached training contract changed: '+name)
        meta.write_text(json.dumps(metadata,indent=2)+'\n');sets=[]
        for label,count,seed in [('train',a.training_rays,39913),('calibration',a.rays,65239),('test',a.rays,98239)]:
            file=folder/(label+'.npz')
            print(name,label,'physical reference',count,flush=True)
            if reuse and file.exists():
                d=np.load(file);q,ids,o,s,x=[d[k] for k in ('q','ids','o','s','x')]
            else:
                q,ids=requests(p,count,seed+ident)
                rng=np.random.default_rng(seed+ident+101)
                q[:,6]=rng.choice([5,9,16,18,32,64],count,p=[.05,.2,.4,.1,.1,.15])
                q[:,5]=np.minimum(q[:,5],min(.3,8*abs(p.scale)))
                # Include the actual unbounded directional-light query heavily.
                q[:count//2,5]=0;q[:count//2,6]=16
                o,s,x=native.evaluate(p,q,ids,model,3,.5)
                np.savez(file,q=q,ids=ids,o=o,s=s,x=x)
            sets.append((q,ids,o,s,x))
        q,ids,o,s,x=sets[0];valid=(s[:,10]>0)&(o[:,2]>=0)
        if valid.sum()<64:raise ValueError('Too few trainable descriptors: '+name)
        print(name,'training',int(valid.sum()),'descriptors',flush=True)
        file=folder/'weights.npz'
        if reuse and file.exists():
            d=np.load(file);w=[d[f'arr_{i}'] for i in range(4)]
        else:w=fit(x[valid],o[valid,2],a.steps,7112031+ident);np.savez(file,*w)
        models[name]=w
        co,cs,_=native.evaluate(p,*sets[1][:2],model,3,.5,w);cutoff=threshold(co,cs)
        cutoffs.append(f'static const float Visibility{name}Threshold = {cutoff:.9e}f;')
        entry={'height_sha256':digest(path),'training_rays':len(q),'training_descriptors':int(valid.sum()),
            'classification_threshold':cutoff};report['profiles'][name]=entry
        for label,d in zip(('calibration','held_out'),sets[1:]):
            oo,ss,xx=native.evaluate(p,d[0],d[1],model,3,cutoff,w)
            assert not ss[:,5:7].any() and ss[:,0].max()<=5
            assert np.isin(oo[:,1],[0,1]).all()
            row=classification(oo,ss)
            row['directional']=classification(oo[d[0][:,5]==0],ss[d[0][:,5]==0])
            row['by_samples']={str(n):classification(oo[d[0][:,6]==n],ss[d[0][:,6]==n]) for n in (5,9,16,18,32,64)}
            row['numpy_native_max_probability_delta']=float(abs(predict(xx[ss[:,10]>0],w)-oo[ss[:,10]>0,3]).max(initial=0))
            entry[label]=row
            np.savez(folder/(label+'-evaluation.npz'),q=d[0],ids=d[1],o=oo,s=ss,x=xx)
        print(name,'held-out error',entry['held_out']['total_error_rate'],'cutoff',cutoff,flush=True)
        (a.output/'training.json').write_text(json.dumps(report,indent=2)+'\n')
    weights=shader(models).replace('Visibility','PhysicalVisibility').replace('Tools/level_three.py','Tools/train_physical_visibility.py (physical reach v2)')
    (a.output/'NeuralVisibilityPhysicalWeights.azsli').write_text(weights)
    (a.output/'VisibilityClassificationThresholds.azsli').write_text('// SPDX-License-Identifier: MIT\n#pragma once\n// Physical reach v2; independent calibration partition.\n'+'\n'.join(cutoffs)+'\n')

if __name__=='__main__':main()

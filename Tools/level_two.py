"""Train/evaluate the 19 -> 24 -> 5 multi-candidate predictor on native descriptors."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
os.environ.setdefault("OPENBLAS_NUM_THREADS","2")
import numpy as np
from PIL import Image
from train_welded_tower import sigmoid
from level_one import Evaluator, FP, UP, U, F, load_profiles, outcomes, audit, no_quality_regression
from level_one_reference import trace


def pack(weights):
    return np.ascontiguousarray(np.concatenate((weights[0].T.ravel(),weights[1],weights[2].ravel(),weights[3])),np.float32)


class Native(Evaluator):
    def __init__(self,path):
        super().__init__(path)
        common=self.native.EvaluateLevelOne.argtypes[:12]
        self.native.EvaluateLevelTwo.argtypes=self.native.EvaluateLevelOne.argtypes
        self.native.EvaluateLevelTwo.restype=ctypes.c_bool
        self.native.DescribeRays.argtypes=common+[U,FP,UP]
        self.native.DescribeRays.restype=ctypes.c_bool
        self.native.ProbeLevelTwo.argtypes=common+[U,ctypes.POINTER(F),FP,UP]
        self.native.ProbeLevelTwo.restype=ctypes.c_bool

    def arguments(self,p,d,model):
        return [p.height,*p.atlas,p.scale,p.reference,p.address,p.packed,len(p.packed),d["rays"],d["ids"],len(d["ids"]),model]

    def describe(self,p,d,model,level):
        values=np.zeros((len(d["ids"]),21),np.float32);valid=np.zeros(len(values),np.uint32)
        assert self.native.DescribeRays(*self.arguments(p,d,model),level,values,valid)
        return values,valid.astype(bool)

    def evaluate2(self,p,d,model,budget,weights=None,head=None):
        hits=np.zeros((len(d["ids"]),5),np.float32);stats=np.zeros((len(hits),16),np.uint32)
        w=pack(weights) if weights is not None else None
        pointer=w.ctypes.data_as(ctypes.POINTER(F)) if w is not None else None
        if head is None:
            ok=self.native.EvaluateLevelTwo(*self.arguments(p,d,model),*budget,pointer,hits,stats)
        else:
            ok=self.native.ProbeLevelTwo(*self.arguments(p,d,model),head,pointer,hits,stats)
        assert ok
        return hits,stats


def dataset(native,p,model,count,seed):
    old,details=p.dataset(native.teacher.TraceRays,count,seed,retain_rays=True)
    features,valid=native.describe(p,details,model,2)
    span=features[:,20]
    target=np.clip((details["teacher"][:,1]-features[:,19])/np.maximum(span,1e-7),0,1).astype(np.float32)
    return (features[:,:19].copy(),old[1],target,span,old[-1]),details,valid,old


def correct_teacher_misses(p,data,details,predictions):
    """Resolve suspected false positives independently; never train on test rays."""
    suspect=np.zeros(len(data[1]),bool)
    for hit,stats in predictions:
        suspect|=(~data[1])&(stats[:,2:4].sum(axis=1)>0)
    corrections=[]
    for i in np.flatnonzero(suspect):
        reference=trace(p.height,p.packed[details["ids"][i]],details["rays"][i],
                        p.scale,p.reference,p.address)
        if reference["status"]=="checked" and reference["nearest_t"] is not None:
            previous=details["teacher"][i].tolist()
            details["teacher"][i]=[1,reference["nearest_t"]]
            data[1][i]=True
            corrections.append({"ray":int(i),"previous_teacher":previous,"independent":reference})
    return corrections


def fit(data,steps,callback,seed):
    x,visible,target,span,_=data
    rng=np.random.default_rng(seed)
    w=[rng.normal(0,.16,(19,24)).astype(np.float32),np.full(24,.06,np.float32),
       rng.normal(0,.06,(24,5)).astype(np.float32),np.asarray([-.8,.8,0,0,0],np.float32)]
    moment=[np.zeros_like(v) for v in w];variance=[np.zeros_like(v) for v in w]
    ratio=np.clip(span/.05,1,24).astype(np.float32)
    for step in range(1,steps+1):
        ids=rng.integers(len(x),size=1024)
        hidden=np.maximum(x[ids]@w[0]+w[1],0)
        pred=sigmoid(hidden**2@w[2]+w[3])
        delta=pred[:,:2]-target[ids,None]
        winner=np.argmin(abs(delta),axis=1)
        responsibility=np.full_like(delta,.04)
        responsibility[np.arange(len(ids)),winner]=1
        error=np.zeros_like(pred)
        gradient=np.clip(delta*ratio[ids,None],-.5,.5)*ratio[ids,None]
        error[:,:2]=2*gradient*responsibility*visible[ids,None]*pred[:,:2]*(1-pred[:,:2])
        error[:,2]=.34*(pred[:,2]-visible[ids])
        # Initial scores learn mode membership. Frozen-head calibration below
        # replaces these with actual successful-two-step refinement labels.
        membership=(np.arange(2)[None,:]==winner[:,None])*visible[ids,None]
        error[:,3:]=.06*(pred[:,3:]-membership)
        error/=len(ids)
        layer=(error@w[2].T)*2*hidden
        gradients=(x[ids].T@layer,layer.sum(axis=0),(hidden**2).T@error,error.sum(axis=0))
        learning=.0035 if step<3000 else .0012
        for k,g in enumerate(gradients):
            moment[k]=.9*moment[k]+.1*g;variance[k]=.999*variance[k]+.001*g*g
            w[k]-=learning*(moment[k]/(1-.9**step))/(np.sqrt(variance[k]/(1-.999**step))+1e-8)
        if step in (1200,2500,4500) or step==steps:
            callback(step,[v.copy() for v in w])
    return w


def calibrate(native,p,model,data,details,weights):
    labels=[]
    for head in (0,1):
        hit,stats=native.evaluate2(p,details,model,(2,0,1),weights,head=head)
        labels.append((stats[:,2]>0)&data[1]&(abs(hit[:,1]-details["teacher"][:,1])<=.00025))
    labels=np.column_stack(labels).astype(np.float32)
    hidden=np.maximum(data[0]@weights[0]+weights[1],0)**2
    rng=np.random.default_rng(581991+model)
    head=weights[2][:,3:].copy();bias=weights[3][3:].copy()
    for step in range(600):
        ids=rng.integers(len(hidden),size=512)
        error=(sigmoid(hidden[ids]@head+bias)-labels[ids])/len(ids)
        head-=.08*(hidden[ids].T@error);bias-=.08*error.sum(axis=0)
    weights[2][:,3:]=head;weights[3][3:]=bias
    return labels.mean(axis=0).tolist()


def shader(models):
    lines=["// SPDX-License-Identifier: MIT","#pragma once","// Generated by Tools/level_two.py; native FP32 descriptors, two validated hypotheses."]
    for name,w in models.items():
        for suffix,array in (("W0",w[0].T),("W1",w[2])):
            lines.append(f"static const float L2{name}{suffix}[{len(array)}][{array.shape[1]}] = {{")
            lines += ["    {"+", ".join(f"{float(v):.9e}f" for v in row)+"}," for row in array]
            lines.append("};")
        for suffix,array in (("B0",w[1]),("B1",w[3])):
            lines.append(f"static const float L2{name}{suffix}[{len(array)}] = {{"+", ".join(f"{float(v):.9e}f" for v in array)+"};")
    return "\n".join(lines)+"\n"


def quality_rank(summary):
    return (summary["refined_false_positives"],
            summary["refined_depth_disagreements"]*2+summary["uncovered_teacher_hits"]*3+summary["teacher_hits_using_fill"],
            summary["refined_depth_p99_mm"] or 0,summary["total_iterations"])


def main():
    cli=argparse.ArgumentParser(description=__doc__)
    cli.add_argument("manifest",type=Path);cli.add_argument("binary_dir",type=Path);cli.add_argument("output",type=Path)
    cli.add_argument("--training-rays",type=int,default=230000)
    cli.add_argument("--rays",type=int,default=8000)
    cli.add_argument("--steps",type=int,default=4500)
    cli.add_argument("--profiles",type=int,nargs="+",default=[1,2,3,4])
    cli.add_argument("--reuse-checkpoints",action="store_true",help="Re-evaluate saved checkpoints with current solver/reference checks")
    args=cli.parse_args()
    if min(args.training_rays,args.rays,args.steps)<1 or any(i not in range(1,5) for i in args.profiles):
        cli.error("Positive ray counts/steps and open-shaft profile ids 1-4 are required")
    if args.output.resolve().is_relative_to((Path(__file__).resolve().parents[1]/"Assets").resolve()):
        cli.error("Experiment output must be outside shipping assets")
    args.output.mkdir(parents=True,exist_ok=True)
    native=Native(args.binary_dir)
    report={"architecture":[19,24,5],"quality_budget":[6,4,1],"profiles":{}}
    models={}
    project=Path(__file__).resolve().parents[3]
    runtime_heights=[
        project/"Assets/SilPOMPlanarAB/IrishFieldstoneTowerWelded/wall/height.png",
        project/"Assets/SilPOMPlanarAB/IrishFieldstoneTowerOpenShaft/sculpted-stair-silpom/height.png",
        project/"Assets/SilPOMPlanarAB/IrishFieldstoneTowerOpenShaft/welded-doorway/trim-blocks/height.png",
        project/"Assets/SilPOMPlanarAB/IrishFieldstoneTowerOpenShaft/welded-doorway/reveal/height.png"]
    for ident,name,p,height_path,mesh_path in load_profiles(args.manifest,native):
        if ident not in args.profiles:continue
        short=name.removeprefix("OpenShaft");model=ident+3
        runtime=runtime_heights[ident-1]
        p.height=np.ascontiguousarray(np.asarray(Image.open(runtime),np.float32)/65535)
        p.atlas=np.asarray(p.height.shape[::-1])
        print(short+": generating native descriptors and teacher labels",flush=True)
        train,td,tvalid,_=dataset(native,p,model,args.training_rays,12631+ident)
        selection,sd,svalid,old=dataset(native,p,model,args.rays,192877+ident)
        test,hd,hvalid,_=dataset(native,p,model,args.rays,78306+ident)
        base_hit,base_stats=native.evaluate(p,sd,model,(6,4,1))
        base=outcomes(selection,sd,base_hit,base_stats)
        entry={"runtime_height_sha256":hashlib.sha256(runtime.read_bytes()).hexdigest(),
               "selection_baseline":base,"checkpoints":[],"training_rays":int(tvalid.sum()),
               "training_clip_disagreements":int((~tvalid).sum())}
        report["profiles"][name]=entry
        f1,v1=native.describe(p,sd,model,1)
        delta=abs(f1[v1,:11]-old[0][v1])
        entry["old_python_vs_native_descriptor"]={"p95_abs":float(np.quantile(delta,.95)),
                                                   "max_abs":float(delta.max()),"over_001":int((delta>.001).sum())}
        train=tuple(v[tvalid] if isinstance(v,np.ndarray) else v for v in train)
        td={k:np.ascontiguousarray(v[tvalid]) for k,v in td.items() if k in ("ids","rays","teacher")}
        cal_ids=np.arange(min(16384,len(train[0])))
        cal=tuple(v[cal_ids] if isinstance(v,np.ndarray) else v for v in train)
        cd={k:np.ascontiguousarray(v[cal_ids]) for k,v in td.items()}
        chosen=None;rank=None;evaluations=[]
        def checkpoint(step,w,already_calibrated=False):
            nonlocal chosen,rank
            rates=None if already_calibrated else calibrate(native,p,model,cal,cd,w)
            hits,stats=native.evaluate2(p,sd,model,(6,4,1),w)
            evaluations.append((step,[v.copy() for v in w],hits,stats))
            summary=outcomes(selection,sd,hits,stats)
            eligible=no_quality_regression(summary,base)
            entry["checkpoints"].append({"step":step,"summary":summary,"strict_gate":eligible,
                                         "two_step_success_rates":rates})
            folder=args.output/short/f"step-{step}";folder.mkdir(parents=True,exist_ok=True)
            np.savez(folder/"weights.npz",*w)
            (folder/"weights.azsli").write_text(shader({short:w}))
            candidate_rank=(not eligible,*quality_rank(summary))
            if rank is None or candidate_rank<rank:
                rank=candidate_rank;chosen=[v.copy() for v in w];entry["selected_step"]=step
            print(short,step,"miss",summary["uncovered_teacher_hits"],"depth",summary["refined_depth_disagreements"],
                  "iterations",summary["total_iterations"],"strict",eligible,flush=True)
            (args.output/"training.json").write_text(json.dumps(report,indent=2)+"\n")
        if args.reuse_checkpoints:
            folders=sorted((args.output/short).glob("step-*/weights.npz"),key=lambda path:int(path.parent.name[5:]))
            if not folders:raise ValueError("No saved checkpoints for "+short)
            for path in folders:
                saved=np.load(path);checkpoint(int(path.parent.name[5:]),[saved[f"arr_{i}"] for i in range(4)],True)
        else:
            fit(train,args.steps,checkpoint,7112026+ident)
        entry["selection_reference_corrections"]=correct_teacher_misses(p,selection,sd,[(base_hit,base_stats)]+[(h,s) for _,_,h,s in evaluations])
        base=outcomes(selection,sd,base_hit,base_stats);entry["selection_baseline"]=base
        for row,(_,w,h,s) in zip(entry["checkpoints"],evaluations):
            row["summary"]=outcomes(selection,sd,h,s)
            row["strict_gate"]=no_quality_regression(row["summary"],base)
        selected=min(range(len(evaluations)),key=lambda i:(not entry["checkpoints"][i]["strict_gate"],*quality_rank(entry["checkpoints"][i]["summary"])))
        entry["selected_step"],chosen,_,_=evaluations[selected]
        models[short]=chosen
        entry["held_out"]={};pairs=[]
        for budget in ((2,1,5.),(4,1,1.),(6,4,1.)):
            base_hit,base_stats=native.evaluate(p,hd,model,budget)
            hit,stats=native.evaluate2(p,hd,model,budget,chosen)
            pairs.append((budget,(base_hit,base_stats),(hit,stats)))
        entry["held_out_reference_corrections"]=correct_teacher_misses(p,test,hd,[pair for _,b,c in pairs for pair in (b,c)])
        for budget,b,c in pairs:
            bs,cs=outcomes(test,hd,*b),outcomes(test,hd,*c)
            entry["held_out"][str(budget)]={"baseline":bs,"level2":cs,"strict_gate":no_quality_regression(cs,bs)}
            if budget==(6,4,1.):
                entry["audit"]=audit(p,hd,*c,12)
        print(short,"selected",entry["selected_step"],flush=True)
        (args.output/"training.json").write_text(json.dumps(report,indent=2)+"\n")
        (args.output/"NeuralTowerLevelTwoWeights.azsli").write_text(shader(models))


if __name__=="__main__":main()

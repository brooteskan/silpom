"""Analyze native cache-off/cache-on timestamp captures, including preparation cost."""
import argparse, hashlib, json, statistics
from collections import defaultdict
from pathlib import Path
import analyze_resolve_regions_benchmark as common

def analyze(folder):
    state=common.read(folder/'results.json')
    assert state['passed'] and state['restored'] and state['saved_level_unchanged']
    assert state['comparison']=='face_caches_off_vs_on' and state['controls']['Lighting estimator']==5
    assert state['controls']['Height-derived normals (off = cheaper)'] is True
    grouped={};hardware=set();files=[]
    for run in state['runs']:
        path=folder/run['file'];raw=common.read(path);meta=raw['capture_metadata']
        assert common.sha256(path)==run['sha256'] and meta==run['capture_metadata']
        assert meta['face_caches_enabled']==(run['mode']=='on') and meta['resolve_regions'] is True
        assert meta['inspection'] is False and meta['readback_during_timing'] is False and meta['lighting_estimator']==5
        assert meta['aa_state']==[2,0,0] and meta['viewport']==[1280,720]
        values,leaves=common.capture_values(raw,path)
        prep=[name for name in leaves if name.endswith('.DepthPrePass.SilPOMFaceCache')]
        if run['mode']=='on':
            assert len(prep)==1 and len(leaves[prep[0]])==raw['frames']
            values['preparation_gpu']=leaves[prep[0]]
        else:
            assert len(prep)<=1
            values['preparation_gpu']=leaves[prep[0]] if prep else [0.0]*raw['frames']
            assert len(values['preparation_gpu'])==raw['frames']
        values['depth_plus_preparation']=[a+b for a,b in zip(values['hit_generation_gpu'],values['preparation_gpu'])]
        grouped[(run['view'],run['repeat'],run['mode'])]=values
        hardware.add(raw['hardware']);files.append(dict(file=run['file'],sha256=run['sha256']))
    assert len(hardware)==1 and len(grouped)==len(state['views'])*state['repeats']*2
    out=dict(passed=True,hardware=next(iter(hardware)),samples=2,viewport=[1280,720],controls=state['controls'],
             capture_count=len(files),frames=len(files)*state['frames_per_capture'],raw_files=files,views={},
             results_sha256=common.sha256(folder/'results.json'),
             note='Median native GPU milliseconds. Depth plus preparation sums nonoverlapping leaf passes. Graphics span includes queue gaps, not GPU busy time or FPS.')
    for view in state['views']:
        pool={m:defaultdict(list) for m in ('off','on')};pairs=[]
        for r in range(1,state['repeats']+1):
            for mode in pool:
                for name,vals in grouped[(view,r,mode)].items():pool[mode][name].extend(vals)
            pairs.append({k:statistics.median(grouped[(view,r,'off')][k])/statistics.median(grouped[(view,r,'on')][k]) for k in ('depth_plus_preparation','resolve_gpu','graphics_span')})
        metrics={}
        for key in pool['off']:
            off,on=common.distribution(pool['off'][key]),common.distribution(pool['on'][key])
            a,b=off['median_ms'],on['median_ms']
            metrics[key]=dict(off=off,on=on,saved_ms=a-b,speedup_x=a/b if b else None,reduction_percent=100*(a-b)/a if a else None)
        out['views'][view]=dict(metrics=metrics,paired_speedups=pairs)
    return out

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('folder',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    report=analyze(a.folder);a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    for view,v in report['views'].items():
        print(view, {k:dict(off=round(x['off']['median_ms'],4),on=round(x['on']['median_ms'],4),speedup=x['speedup_x']) for k,x in v['metrics'].items()})

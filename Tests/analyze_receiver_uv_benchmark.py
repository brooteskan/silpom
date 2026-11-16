"""Exact 2x sample ownership/depth, explicit usage, quality and GPU timing."""
import argparse,json,statistics,struct
from pathlib import Path
import numpy as np
from analyze_decal_bake_benchmark import dds,normals,errors


def image(path,kind):
    h,w,fmt,data=dds(path)
    if kind=='records':assert fmt==3;return np.frombuffer(data,dtype='<u4').reshape(h,w,4)
    if kind=='depth':assert fmt==41;return np.frombuffer(data,dtype='<u4').reshape(h,w)
    if kind=='diffuse':assert fmt==10;return np.frombuffer(data,dtype='<f2').reshape(h,w,4).astype(np.float32)[...,:3]
    if kind=='normal':
        assert fmt==10
        channels=np.rint(np.frombuffer(data,dtype='<f2').reshape(h,w,4).astype(np.float32)[...,:3]*1023).astype(np.uint32)
        return channels[...,0]|(channels[...,1]<<10)|(channels[...,2]<<20)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('output',type=Path);root=parser.parse_args().output
    state=json.loads((root/'results.json').read_text());assert state['passed'] and state['saved_level_unchanged'],state
    assert state.get('no_skipped_resource_bindings') and not state.get('error'),state
    result={'state':state,'views':{},'timings':{}}
    for path in sorted(root.glob('*-gpu.json')):
        d=json.loads(path.read_text());assert d['complete'] and d['depth_attachment']=={'width':1280,'height':720,'msaa':2}
        ids={i for i,p in enumerate(d['passes']) if p['path'].endswith('.SilPOMHitForward')}
        assert not any('UvUsage' in p['path'] and p.get('enabled',False) for p in d['passes'])
        result['timings'][path.stem.removesuffix('-gpu')]={'gpu_span_ms':statistics.median(v/1e6 for v in d['graphics_span_ns'] if v),
            'hit_forward_ms':statistics.median(v[4]/1e6 for v in d['samples'] if v[1] in ids),
            'cpu_frame_period_ms':statistics.median(v for v in d['cpu_frame_ms'] if v)}
    for name in ['close','ordinary','minified']:
        ref=root/(name+'-0-0');base=image(Path(str(ref)+'-records.dds'),'records');depth=image(Path(str(ref)+'-depth.dds'),'depth')
        assert base.shape[1]==1280*4
        baseline=image(Path(str(ref)+'-diffuse-samples.dds'),'diffuse');normalBase=image(Path(str(ref)+'-normal-samples.dds'),'normal')
        assert not np.any(image(Path(str(ref)+'-usage.dds'),'records')),'Original path reported a cache attempt'
        comparisons={}
        for suffix in ['1-1','2-2']:
            stem=root/(name+'-'+suffix);record=image(Path(str(stem)+'-records.dds'),'records');z=image(Path(str(stem)+'-depth.dds'),'depth')
            use=image(Path(str(stem)+'-usage.dds'),'records');color=image(Path(str(stem)+'-diffuse-samples.dds'),'diffuse')
            packed=image(Path(str(stem)+'-normal-samples.dds'),'normal');sampleResults=[]
            for sample in [0,1]:
                a=base[:,sample::4];b=record[:,sample::4];u=use[:,sample::4];mask=(a[...,3]&3)==1
                assert np.array_equal(a,b),(name,suffix,sample,'hit record changed')
                assert np.array_equal(depth[:,sample::4],z[:,sample::4]),(name,suffix,sample,'depth changed')
                assert np.all(u[...,0]<=2)
                attempted=u[...,0]!=0
                assert np.all(u[...,1][attempted]==state['receiver']['hit_face'])
                assert np.all(u[...,2][attempted]==2048)
                accepted=u[...,0]==2
                other=mask&((a[...,3]>>2)!=state['receiver']['hit_face'])
                assert np.array_equal(baseline[:,sample::4][other],color[:,sample::4][other]),'Paint changed a different receiver face'
                sampleResults.append({'sample':sample,'hit_records_bit_exact':True,'depth_bit_exact':True,
                    'no_attempt':int((u[...,0]==0).sum()),'prefix_replay':int((u[...,0]==1).sum()),'cache_used':int(accepted.sum()),
                    'diffuse_error':errors(baseline[:,sample::4],color[:,sample::4],mask),
                    'cached_diffuse_error':errors(baseline[:,sample::4],color[:,sample::4],accepted)})
            if suffix=='2-2':assert all(s['cache_used']>0 for s in sampleResults),(name,'Cache was not actually used by both samples')
            n0=normals(normalBase);n1=normals(packed);angle=np.where(normalBase==packed,0,np.degrees(np.arccos(np.clip(np.sum(n0*n1,axis=-1),-1,1))))
            for sample in [0,1]:
                cached=use[:,sample::4,0]==2
                sampleResults[sample]['cached_normal_degrees']=errors(np.zeros_like(angle[:,sample::4]),angle[:,sample::4],cached)
            comparisons[suffix]={'samples':sampleResults}
        result['views'][name]=comparisons
    zero=image(root/'zero-prefix-usage.dds','records');assert not np.any(zero),'Zero-prefix case falsely reported cache/replay activity'
    assert not np.any(image(root/'orphan-surface-usage.dds','records')),'Orphaned stamps became active projections'
    live=image(root/'live-tail-edited-diffuse-samples.dds','diffuse');cached=image(root/'close-2-2-diffuse-samples.dds','diffuse')
    assert np.any(live!=cached),'Live tail edit did not affect rendered color'
    tail=image(root/'live-tail-only-diffuse-samples.dds','diffuse')
    use=image(root/'close-2-2-usage.dds','records')[...,0]==2
    visible=use&np.any(np.abs(cached-tail)>1e-4,axis=-1)
    real_visible=int(visible[:,0::4].sum()+visible[:,1::4].sum())
    result['visible_fixed_paint_samples']=real_visible;assert real_visible>100,'Cache was used but fixed paint made no visible contribution'
    result['zero_prefix_no_attempt']=True
    for name in ['close','ordinary','minified']:
        for mode in [0,1,2]:
            samples=[d for key,d in result['timings'].items() if key.startswith(name+'-') and key.endswith('-'+str(mode))]
            result.setdefault('aggregate',{}).setdefault(name,{})[str(mode)]={k:statistics.mean(d[k] for d in samples) for k in samples[0]}
    (root/'analysis.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'aggregate':result['aggregate'],'views':result['views'],'zero_prefix_no_attempt':True},indent=2))


if __name__=='__main__':main()

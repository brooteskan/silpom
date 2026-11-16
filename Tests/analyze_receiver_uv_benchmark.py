"""Exact 2x sample ownership/depth, explicit usage, quality and GPU timing."""
import argparse,json,statistics,struct
from pathlib import Path
import numpy as np
from analyze_decal_bake_benchmark import dds,normals,errors


def normal_errors(before,after,mask):
    n0=normals(before);n1=normals(after)
    angle=np.where(before==after,0,np.degrees(np.arccos(np.clip(np.sum(n0*n1,axis=-1),-1,1))))
    return errors(np.zeros_like(angle),angle,mask)


def compare_sample(base,record,depth,z,baseline,color,normal_base,packed,use,face,fixed,mode):
    mask=(base[...,3]&3)==1
    assert np.array_equal(base,record),'hit record changed'
    assert np.array_equal(depth,z),'depth changed'
    assert np.all(use[...,0]<=2),'Unknown prefix usage code'
    attempted=use[...,0]!=0
    assert np.all(mask[attempted]),'Prefix attempted outside a receiving hit'
    assert np.all(use[...,1][attempted]==face),'Prefix attempted on a different face'
    assert np.all((base[...,3]>>2)[attempted]==face),'Usage record disagrees with hit face'
    assert np.all(use[...,2][attempted]==fixed),'Wrong prefix length'
    replay=use[...,0]==1;accepted=use[...,0]==2
    assert np.any(accepted if mode==2 else replay),'Prefix was not exercised by this sample'
    if mode==1:
        assert not np.any(accepted),'Replay mode unexpectedly used an atlas'
        assert np.array_equal(baseline[mask],color[mask]),'Prefix replay changed native color'
        assert np.array_equal(normal_base[mask],packed[mask]),'Prefix replay changed native normals'
    other=mask&((base[...,3]>>2)!=face)
    assert np.array_equal(baseline[other],color[other]),'Paint changed a different receiver face color'
    assert np.array_equal(normal_base[other],packed[other]),'Paint changed a different receiver face normal'
    # Approximation is allowed only where the atlas was actually sampled.
    # Include usage==0 receiving pixels as well as explicit prefix replay.
    non_atlas=mask&~accepted
    if mode==2:
        assert np.array_equal(baseline[non_atlas],color[non_atlas]),'Atlas fallback changed native color'
        assert np.array_equal(normal_base[non_atlas],packed[non_atlas]),'Atlas fallback changed native normals'
    assert np.all(np.isfinite(color[mask])),'Nonfinite diffuse output'
    result={'hit_records_bit_exact':True,'depth_bit_exact':True,'other_face_color_and_normal_bit_exact':True,
        'non_atlas_receiving_pixels':int(non_atlas.sum()),'non_atlas_color_and_normal_bit_exact':True,
        'no_attempt':int((use[...,0]==0).sum()),'prefix_replay':int(replay.sum()),'cache_used':int(accepted.sum()),
        'diffuse_error':errors(baseline,color,mask),'normal_degrees':normal_errors(normal_base,packed,mask),
        'owned_diffuse_error':errors(baseline,color,attempted),'owned_normal_degrees':normal_errors(normal_base,packed,attempted),
        'prefix_replay_normal_degrees':normal_errors(normal_base,packed,replay),
        'cached_diffuse_error':errors(baseline,color,accepted),'cached_normal_degrees':normal_errors(normal_base,packed,accepted)}
    c=result['cached_diffuse_error'] if mode==2 else result['owned_diffuse_error']
    n=result['cached_normal_degrees'] if mode==2 else result['owned_normal_degrees']
    result['quality_passed']=c['pixels']>0 and n['pixels']>0 and c['rms']<=.01 and c['maximum']<=.05 and n['rms']<=1 and n['maximum']<=5
    return result


def compare_no_paint(base,record,depth,z,baseline,color,normal_base,packed,use):
    assert not np.any(use),'Empty/orphan paint reported cache/replay activity'
    assert np.array_equal(base,record),'Empty/orphan paint changed hit records'
    assert np.array_equal(depth,z),'Empty/orphan paint changed depth'
    assert np.any((base[...,3]&3)==1),'No-paint baseline has no receiving surface'
    assert np.array_equal(baseline,color),'Empty/orphan paint retained visible color'
    assert np.array_equal(normal_base,packed),'Empty/orphan paint retained visible normals'
    return {'hit_records_bit_exact':True,'depth_bit_exact':True,'color_and_normal_bit_exact':True,'no_attempt':True}


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
    assert state.get('no_paint_baseline_before_fixture'),'Recapture with a no-paint baseline before submitting fixture stamps'
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
                s=np.s_[:,sample::4]
                metrics=compare_sample(base[s],record[s],depth[s],z[s],baseline[s],color[s],normalBase[s],packed[s],use[s],
                    state['receiver']['hit_face'],state['fixed'],int(suffix[-1]))
                sampleResults.append({'sample':sample,**metrics})
            comparisons[suffix]={'samples':sampleResults}
        result['views'][name]=comparisons
    def load(stem,label,kind):return image(root/(stem+'-'+label+'.dds'),kind)
    result['no_paint_checks']={}
    for stem in ['orphan-surface','zero-prefix']:
        samples=[]
        for sample in [0,1]:
            s=np.s_[:,sample::4]
            pairs=[(load('no-paint',label,kind)[s],load(stem,label,kind)[s]) for label,kind in
                [('records','records'),('depth','depth'),('diffuse-samples','diffuse'),('normal-samples','normal')]]
            samples.append({'sample':sample,**compare_no_paint(*(v for pair in pairs for v in pair),load(stem,'usage','records')[s])})
        result['no_paint_checks'][stem]={'samples':samples}
    live=image(root/'live-tail-edited-diffuse-samples.dds','diffuse');cached=image(root/'close-2-2-diffuse-samples.dds','diffuse')
    assert np.any(live!=cached),'Live tail edit did not affect rendered color'
    tail=image(root/'live-tail-only-diffuse-samples.dds','diffuse')
    use=image(root/'close-2-2-usage.dds','records')[...,0]==2
    visible=use&np.any(np.abs(cached-tail)>1e-4,axis=-1)
    real_visible=int(visible[:,0::4].sum()+visible[:,1::4].sum())
    result['visible_fixed_paint_samples']=real_visible;assert real_visible>100,'Cache was used but fixed paint made no visible contribution'
    result['zero_prefix_no_attempt']=True
    result['quality_checks']={name:{mode:all(s['quality_passed'] for s in data['samples']) for mode,data in comparisons.items()}
        for name,comparisons in result['views'].items()}
    for name in ['close','ordinary','minified']:
        for mode in [0,1,2]:
            samples=[d for key,d in result['timings'].items() if key.startswith(name+'-') and key.endswith('-'+str(mode))]
            result.setdefault('aggregate',{}).setdefault(name,{})[str(mode)]={k:statistics.mean(d[k] for d in samples) for k in samples[0]}
    (root/'analysis.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:result[k] for k in ['aggregate','views','quality_checks','no_paint_checks','zero_prefix_no_attempt']},indent=2))


if __name__=='__main__':main()

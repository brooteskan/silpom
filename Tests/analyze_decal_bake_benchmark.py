"""Measure bake GPU cost, finite-resolution error, and actual cache coverage."""
import argparse
import json
from pathlib import Path
import statistics
import struct
import numpy as np


def dds(path):
    data=path.read_bytes()
    assert data[:4]==b'DDS ' and data[84:88]==b'DX10',path
    h,w=struct.unpack_from('<II',data,12)
    fmt=struct.unpack_from('<I',data,128)[0]
    return h,w,fmt,data[148:]


def normals(packed):
    v=packed.astype(np.uint32)
    r=(v&1023).astype(np.float32)/1023;g=((v>>10)&1023).astype(np.float32)/1023;b=((v>>20)&1023).astype(np.float32)/1023
    x=r-g;y=r+g-1;z=(b*2-1)*(1-np.abs(x)-np.abs(y))
    n=np.stack((x,y,z),axis=-1)
    return n/np.maximum(np.linalg.norm(n,axis=-1,keepdims=True),1e-12)


def errors(a,b,mask):
    delta=np.abs(a-b)[mask]
    return {'pixels':int(mask.sum()),'mean':float(delta.mean()) if delta.size else 0,
        'rms':float(np.sqrt(np.mean(delta*delta))) if delta.size else 0,
        'p99':float(np.quantile(delta,.99)) if delta.size else 0,'maximum':float(delta.max()) if delta.size else 0}


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);args=p.parse_args();root=args.output
    state=json.loads((root/'results.json').read_text());assert state['passed'] and state['saved_level_unchanged'],state
    report={'state':state,'timings':{},'comparisons':{}}
    for path in sorted(root.glob('*-gpu.json')):
        d=json.loads(path.read_text());assert d['complete'] and d['frames']==180
        assert d['depth_attachment']=={'width':1280,'height':720,'msaa':2}
        hit_ids={i for i,v in enumerate(d['passes']) if v['path'].endswith('.SilPOMHitForward')}
        report['timings'][path.name.removesuffix('-gpu.json')]={'gpu_span_ms':statistics.median(v/1e6 for v in d['graphics_span_ns'] if v),
            'hit_forward_ms':statistics.median(v[4]/1e6 for v in d['samples'] if v[1] in hit_ids),
            'cpu_frame_period_ms':statistics.median(v for v in d['cpu_frame_ms'] if v)}
    for path in sorted(root.glob('*-0-0-hit.dds')):
        stem=path.name.removesuffix('-0-0-hit.dds');a=root/(stem+'-0-0');b=root/(stem+'-1-1')
        ah=dds(Path(str(a)+'-hit.dds'))[3];bh=dds(Path(str(b)+'-hit.dds'))[3]
        hits=np.frombuffer(ah,dtype='<u4').reshape(-1,4);mask=(hits[:,3]&3)==1
        ca=dds(Path(str(a)+'-diffuse.dds'));cb=dds(Path(str(b)+'-diffuse.dds'));assert ca[2]==cb[2]==10
        c0=np.frombuffer(ca[3],dtype='<f2').reshape(-1,4).astype(np.float32)[:,:3]
        c1=np.frombuffer(cb[3],dtype='<f2').reshape(-1,4).astype(np.float32)[:,:3]
        na=dds(Path(str(a)+'-normal.dds'));nb=dds(Path(str(b)+'-normal.dds'));assert na[2]==nb[2]==24
        p0=np.frombuffer(na[3],dtype='<u4');p1=np.frombuffer(nb[3],dtype='<u4')
        n0=normals(p0);n1=normals(p1)
        angle=np.where(p0==p1,0,np.degrees(np.arccos(np.clip(np.sum(n0*n1,axis=1),-1,1))))
        r={'hit_records_identical':ah==bh,'diffuse_linear':errors(c0,c1,mask),'normal_degrees':errors(np.zeros_like(angle),angle,mask)}
        assert ah==bh,'Baking changed geometry/hit records: '+stem
        assert np.all(np.isfinite(c1[mask])) and np.all(np.isfinite(n1[mask])),stem
        usage=Path(str(b)+'-usage.dds')
        if usage.exists():
            image=np.frombuffer(dds(usage)[3],dtype='<f2').reshape(-1,4).astype(np.float32)[:,:3]
            positive=image>1e-6
            categories={'baked':mask&positive[:,2]&~positive[:,0]&~positive[:,1],
                'empty':mask&positive[:,1]&~positive[:,0]&~positive[:,2],
                'fallback':mask&positive[:,0]&~positive[:,1]&~positive[:,2]}
            r['coverage']={k:int(v.sum()) for k,v in categories.items()}
            reject=Path(str(b)+'-reject.dds')
            if reject.exists():
                rgb=np.frombuffer(dds(reject)[3],dtype='<f2').reshape(-1,4).astype(np.float32)[:,:3]>1e-6
                code=rgb[:,0].astype(np.uint8)+rgb[:,1]*2+rgb[:,2]*4
                labels={1:'footprint',2:'attenuation_error',3:'receiver_normal',4:'normal_dependence',5:'bounds',6:'nonfinite_or_hdr',7:'incoming_normal_cone'}
                r['rejection_reasons']={name:int((categories['fallback']&(code==value)).sum()) for value,name in labels.items()}
            r['baked_diffuse_linear']=errors(c0,c1,categories['baked'])
            r['baked_normal_degrees']=errors(np.zeros_like(angle),angle,categories['baked'])
            if (root/(stem+'-no-paint-diffuse.dds')).exists():
                hidden=np.frombuffer(dds(root/(stem+'-no-paint-diffuse.dds'))[3],dtype='<f2').reshape(-1,4).astype(np.float32)[:,:3]
                r['visible_paint_pixels']=int((mask&np.any(np.abs(c0-hidden)>1e-4,axis=1)).sum())
                painted=mask&np.any(np.abs(c0-hidden)>1e-4,axis=1)
                hiddenNormal=root/(stem+'-no-paint-normal.dds')
                if hiddenNormal.exists():
                    packed=np.frombuffer(dds(hiddenNormal)[3],dtype='<u4')
                    hiddenN=normals(packed)
                    normalChange=np.where(p0==packed,0,np.degrees(np.arccos(np.clip(np.sum(n0*hiddenN,axis=1),-1,1))))
                    r['visible_normal_paint_pixels']=int((mask&(normalChange>.1)).sum())
                    painted=painted|(mask&(normalChange>.1))
                bakedPaint=painted&categories['baked']
                r['baked_visible_paint_pixels']=int(bakedPaint.sum())
                r['painted_diffuse_linear']=errors(c0,c1,painted)
                r['painted_normal_degrees']=errors(np.zeros_like(angle),angle,painted)
                r['baked_painted_diffuse_linear']=errors(c0,c1,bakedPaint)
                r['baked_painted_normal_degrees']=errors(np.zeros_like(angle),angle,bakedPaint)
        report['comparisons'][stem]=r
    groups={}
    for name,metrics in report['timings'].items():
        fixture_view,repeat,mode=name.rsplit('-',2)
        groups.setdefault((fixture_view,mode),[]).append(metrics)
    report['aggregate']={}
    for view,mode in groups:
        if mode!='0' or (view,'1') not in groups:continue
        before={k:statistics.median(run[k] for run in groups[(view,'0')]) for k in groups[(view,'0')][0]}
        after={k:statistics.median(run[k] for run in groups[(view,'1')]) for k in groups[(view,'1')][0]}
        report['aggregate'][view]={'off':before,'on':after,'percent':{k:(after[k]/before[k]-1)*100 for k in before}}
    report['quality_checks']={}
    for name,r in report['comparisons'].items():
        if r.get('baked_visible_paint_pixels',0)>0:
            color=r['baked_painted_diffuse_linear'];normal=r['baked_painted_normal_degrees']
            report['quality_checks'][name]=color['rms']<=.01 and color['maximum']<=.05 and normal['rms']<=1 and normal['maximum']<=5
    patch=report['comparisons'].get('patch-patch')
    report['paint_cache_exercised']=patch is not None and patch.get('baked_visible_paint_pixels',0)>0
    (root/'analysis.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:report[k] for k in ('aggregate','comparisons','quality_checks','paint_cache_exercised')},indent=2))


if __name__=='__main__':main()

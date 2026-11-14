"""Verify sample ownership/depth, protected coverage and shaded-color resolve."""
import json
from pathlib import Path
import struct
import sys
import numpy as np
from PIL import Image
from NearestHitVisualTests import compare


def dds(path):
    raw=path.read_bytes();assert raw[:4]==b'DDS ' and raw[84:88]==b'DX10'
    height,width=struct.unpack_from('<II',raw,12);fmt=struct.unpack_from('<I',raw,128)[0]
    dtype,channels={3:('<u4',4),10:('<f2',4),2:('<f4',4),41:('<f4',1)}[fmt]
    x=np.frombuffer(raw,dtype=dtype,offset=148).reshape(height,width,channels)
    return x


def atlas(path):
    x=dds(path);assert x.shape[1]%4==0
    return x.reshape(x.shape[0],x.shape[1]//4,4,x.shape[2])


def sample_views(root,name,hits,depth):
    h,w=hits.shape[:2];image=np.zeros((h*2,w*2,3),np.uint8)
    for s in range(4):
        owner=hits[:,:,s,3]>>2;key=owner*2654435761
        rgb=np.stack(((key>>0)&255,(key>>8)&255,(key>>16)&255),axis=-1).astype(np.uint8)
        empty=(hits[:,:,s,3]&3)==0
        rgb[empty & (depth[:,:,s,0]>0)]=[70,100,150]  # ordinary scene geometry
        rgb[empty & (depth[:,:,s,0]==0)]=[255,255,255]  # background
        image[(s//2)*h:(s//2+1)*h,(s%2)*w:(s%2+1)*w]=rgb
    Image.fromarray(image).save(root/f'ownership-{name}.png')


def validate(root):
    compare(root)
    report=json.loads((root/'results.json').read_text());assert report['saved_level_unchanged']
    summary={};partial_total=mixed_total=ordinary_total=0
    for name in ('lower-glancing','lower-facing','upper-glancing'):
        def records(state,stage):return atlas(root/f'{name}-{state}-SilPOMInspect{stage}-Records.dds')
        def depths(state,stage):return atlas(root/f'{name}-{state}-SilPOMInspect{stage}-SampleDepth.dds')
        raw=records('raw','Raw');untouched=records('raw','Final');raw_depth=depths('raw','Raw')
        assert np.array_equal(raw,untouched),'Raw mode modified sample ownership'
        assert np.array_equal(raw,records('repaired','Raw')),'Raw sample intersections changed between repair modes'
        final=records('repaired','Final');final_depth=depths('repaired','Final')
        valid=(raw[:,:,:,3]&3)==1;owner=raw[:,:,:,3]>>2
        partial=(valid.sum(2)>0)&(valid.sum(2)<4)
        mixed=(valid.all(2)) & (owner.max(2)!=owner.min(2))
        protected=partial|mixed
        assert np.array_equal(raw[protected],final[protected]),'Repair changed legitimate mixed/partial coverage'
        assert np.array_equal(raw_depth[protected],final_depth[protected]),'Repair overwrote mixed/partial scene depth'
        ordinary=(~valid)&(raw_depth[:,:,:,0]>0)
        assert np.array_equal(raw_depth[ordinary],final_depth[ordinary]),'Repair overwrote closer ordinary geometry'
        for record,depth in ((raw,raw_depth),(final,final_depth)):
            owned=(record[:,:,:,3]&3)==1
            z=(record[:,:,:,2]&0x3fffffff).copy().view('<f4')
            assert np.array_equal(z[owned],depth[:,:,:,0][owned]),'Hit depth does not match this sample scene depth'
            uv=record[:,:,:,:2].copy().view('<f4');assert np.isfinite(uv[owned]).all()
        # Distinct camera rays must produce distinct UVs on fully covered faces.
        same=valid.all(2)&(owner.max(2)==owner.min(2))
        assert same.sum()>1000
        uv=raw[:,:,:,:2].copy().view('<f4');distinct=np.any(np.ptp(uv,axis=2)>1e-7,axis=2)
        assert distinct[same].mean()>.95,'Samples reused the pixel-center hit'
        colors=atlas(root/f'{name}-repaired-DiffuseSamples.dds').astype(float)
        resolved=dds(root/f'{name}-repaired-ResolvedDiffuse.dds').astype(float)
        assert np.isfinite(colors).all() and np.isfinite(resolved).all()
        np.testing.assert_allclose(resolved,colors.mean(2),atol=.0005,rtol=.0015,
            err_msg='Resolved lighting differs from mean of independently shaded samples')
        count=int(partial.sum());partial_total+=count;mixed_total+=int(mixed.sum());ordinary_total+=int(ordinary.sum())
        sample_views(root,name,raw,raw_depth)
        summary[name]=dict(partial_pixels=count,mixed_owner_pixels=int(mixed.sum()),ordinary_samples=int(ordinary.sum()),
            distinct_hit_fraction=float(distinct[same].mean()),color_resolve_max_error=float(np.abs(resolved-colors.mean(2)).max()))
        # A display-space supersampled diagnostic, with LOD disabled in capture.
        source=Image.open(root/f'supersample2x-{name}.png').convert('RGB')
        assert source.size==(2560,1440)
        target=source.resize((1280,720),Image.Resampling.LANCZOS);target.save(root/f'supersampled-reference-{name}.png')
        reference=np.asarray(target).astype(float);edge=protected
        errors={}
        for mode in ('none','SMAA','TAA','msaa4'):
            a=np.asarray(Image.open(root/f'{mode}-{name}.png').convert('RGB')).astype(float)
            errors[mode]=float(np.abs(a-reference)[edge].mean())
        summary[name]['display_space_edge_mae_vs_supersample']=errors
    assert partial_total>100,'Fixture never exercised partial sample coverage'
    assert mixed_total>10,'Fixture never exercised distinct owners in one pixel'
    assert ordinary_total>100,'Fixture never exercised ordinary geometry occlusion'
    saved=atlas(root/'saved-msaa4-SilPOMInspectFinal-Records.dds')
    saved_depth=atlas(root/'saved-msaa4-SilPOMInspectFinal-SampleDepth.dds')[:,:,:,0]
    owned=(saved[:,:,:,3]&3)==1
    assert owned.sum()>1000
    assert np.array_equal((saved[:,:,:,2]&0x3fffffff).copy().view('<f4')[owned],saved_depth[owned])
    saved_colors=atlas(root/'saved-msaa4-DiffuseSamples.dds').astype(float)
    np.testing.assert_allclose(dds(root/'saved-msaa4-ResolvedDiffuse.dds'),saved_colors.mean(2),atol=.0005,rtol=.0015)
    summary['saved-settings-msaa4']=dict(owned_samples=int(owned.sum()))
    for name in ('proxy-plane','proxy-depth'):
        hits=atlas(root/f'{name}-SilPOMInspectFinal-Records.dds')
        depth=atlas(root/f'{name}-SilPOMInspectFinal-SampleDepth.dds')[:,:,:,0]
        valid=(hits[:,:,:,3]&3)==1
        selected=np.isin(hits[:,:,:,3]>>2,report['proxy_cases'][name]['owners'])&valid
        proxy=selected&((hits[:,:,:,2]&0xc0000000)==0x80000000)
        assert proxy.sum()>100,(name,'No visible selected proxy samples')
        z=(hits[:,:,:,2]&0x3fffffff).copy().view('<f4')
        assert np.array_equal(z[valid],depth[valid]),name+' sample depth mismatch'
        assert np.isfinite(hits[:,:,:,:2].copy().view('<f4')[valid]).all()
        summary[name]=dict(proxy_samples=int(proxy.sum()),owned_samples=int(valid.sum()))
    dark=atlas(root/'environment-dark-DiffuseSamples.dds').astype(float)
    lit=atlas(root/'environment-lit-DiffuseSamples.dds').astype(float)
    resolved=dds(root/'environment-lit-ResolvedDiffuse.dds').astype(float)
    hits=atlas(root/'environment-lit-SilPOMInspectFinal-Records.dds')
    owned=(hits[:,:,:,3]&3)==1
    assert owned.sum()>1000 and np.isfinite(lit).all()
    assert (lit[:,:,:,:3]-dark[:,:,:,:3])[owned].mean()>.001,'Indirect fixture supplied no measurable illumination'
    np.testing.assert_allclose(resolved,lit.mean(2),atol=.0005,rtol=.0015,
        err_msg='Indirect lighting resolved geometry attributes before sample shading')
    summary['indirect']=dict(mean_added_light=float((lit[:,:,:,:3]-dark[:,:,:,:3])[owned].mean()),
        resolve_max_error=float(np.abs(resolved-lit.mean(2)).max()))
    states=report.get('aa_pipeline_states')
    if states is not None:
        assert states['SMAA']==[1,1,1],'SMAA requires the SMAA pipeline mode and enabled feature processor at 1x'
        summary['smaa_pipeline_state']=states['SMAA']
    else:
        # Preserve validation of older captures that used timing as activation
        # evidence. New visual runs do not require performance capture files.
        smaa=json.loads((root/'gpu-SMAA.json').read_text())
        active={p['path']:sum(s[1]==i for s in smaa['samples']) for i,p in enumerate(smaa['passes']) if 'SMAA' in p['path']}
        assert any('BlendingWeight' in name and count>=100 for name,count in active.items()),'SMAA was only a pass-through comparison'
        summary['smaa_active_gpu_samples']=active
    (root/'msaa-validation.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2))


if __name__=='__main__':validate(Path(sys.argv[1]))

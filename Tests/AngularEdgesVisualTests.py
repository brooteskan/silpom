"""Validate the isolated near-wall capture, including diffuse-only AO routing."""
import json
from pathlib import Path
import struct
import sys
import numpy as np
from PIL import Image
from NearestHitVisualTests import compare


def attachment(path):
    raw = path.read_bytes()
    assert raw[:4] == b'DDS ' and raw[84:88] == b'DX10'
    fmt = struct.unpack_from('<I', raw, 128)[0]
    if fmt in (2, 3, 10, 28, 29, 87, 91):
        dtype = {2:'<f4',3:'<u4',10:'<f2'}.get(fmt, 'u1')
        return np.frombuffer(raw, dtype=dtype, offset=148).reshape(-1,4).copy()
    if fmt == 24:
        x = np.frombuffer(raw, dtype='<u4', offset=148)
        return np.stack((x&1023,(x>>10)&1023,(x>>20)&1023,x>>30),axis=-1)
    raise AssertionError(('Unsupported format', fmt))


def hit(path):
    x = attachment(path)
    x[:,3] &= 3
    return x


def wall_mask(h):
    # This flag is written exclusively for canonical trained wall model 4.
    # Exclude proxies, generic geometry and background, even if they are lit.
    return ((h[:,3]&3)==1) & ((h[:,2]&0xc0000000)==0x40000000)


def ambient(root,mask,metrics):
    off,on,restored=(attachment(root/f'environment-{s}-Indirect.dds')[mask,:3].astype(float)
                     for s in ('off','on','restored'))
    assert np.isfinite(off).all() and np.isfinite(on).all()
    lit=off.mean(1)>.001
    assert lit.sum()>1000, 'Ambient fixture has no nonzero indirect wall illumination'
    assert np.allclose(off,restored,atol=.002,rtol=.002), 'Indirect fixture failed to restore'
    assert (on[lit]<=off[lit]+.002).all(), 'Accessibility increased indirect lighting'
    albedo_off,albedo_on=(attachment(root/f'environment-{s}-AlbedoOutput.dds')[mask,:3].astype(float)
                         for s in ('off','on'))
    gain=albedo_on/np.maximum(albedo_off,1)
    affected=lit & (gain.mean(1)<.98)
    assert affected.sum()>1000, 'No illuminated receiver region exercises accessibility'
    # The actual opaque GI composite must respond to its albedo input, rather
    # than merely exporting a darker buffer into an otherwise black frame.
    error=np.abs(on-off*gain)[lit].sum()/off[lit].sum()
    assert error<.01, ('Indirect light did not follow diffuse accessibility',error)
    assert on[affected].mean()<off[affected].mean()*.98, 'No indirect illumination reduction'
    images=[np.asarray(Image.open(root/f'environment-{s}.png').convert('RGB')).reshape(-1,3)[mask].astype(float)
            for s in ('off','on')]
    delta=images[0].mean(1)-images[1].mean(1)
    assert (delta[affected]>1).sum()>1000, 'Ambient effect is not visible in the receiver image'
    metrics['ambient']=dict(illuminated_wall_pixels=int(lit.sum()),affected_wall_pixels=int(affected.sum()),
        indirect_mean_off=float(off[lit].mean()),indirect_mean_on=float(on[lit].mean()),
        relative_composite_routing_error=float(error),visible_darkened_pixels=int((delta[affected]>1).sum()))


def moving(root,report,metrics):
    fixture=json.loads(Path(__file__).with_name('Reference').joinpath('AngularEdgesEngineReceivers.json').read_text())
    offsets=np.asarray(report['moving_light_offsets_degrees']);assert np.array_equal(offsets,np.arange(-18,19))
    first=hit(root/'moving-5-00-Hit.dds');mask=wall_mask(first);assert mask.sum()>1000
    for receiver in fixture['receivers']:
        x,y=receiver['center'];uv=first[y*1280+x,:2].copy().view('<f4')
        np.testing.assert_allclose(uv,receiver['uv'],atol=2e-5,rtol=0,err_msg='Frozen wall receiver moved')
    radiance={mode:[] for mode in (0,3,5)};wall_sums={mode:[] for mode in (0,3,5)}
    contrast=[]
    for i in range(len(offsets)):
        frame={}
        for mode in (0,3,5):
            h=hit(root/f'moving-{mode}-{i:02d}-Hit.dds')
            assert np.array_equal(first,h), 'Light or estimator changed geometry'
            image=attachment(root/f'moving-{mode}-{i:02d}-DiffuseOutput.dds').astype(float)
            assert np.isfinite(image).all()
            wall_sums[mode].append(float(image[mask,:3].sum()))
            frame[mode]=image.reshape(720,1280,4)
            patches=[]
            for receiver in fixture['receivers']:
                x,y=receiver['center'];r=receiver['radius']
                region=np.s_[y-r:y+r+1,x-r:x+r+1]
                assert mask.reshape(720,1280)[region].all(), 'Frozen receiver no longer lands on the wall'
                patches.append(float(frame[mode][region][:,:,:3].mean()))
            radiance[mode].append(patches)
        a,b,base=(frame[mode].reshape(-1,4)[mask,:3].mean(1) for mode in (3,5,0))
        lit=base>.01
        assert lit.sum()>1000, 'Moving light did not illuminate intended wall receivers'
        contrast.append(int((np.abs(a[lit]-b[lit])>.01).sum()))
    radiance={mode:np.asarray(v).T for mode,v in radiance.items()}
    assert min(radiance[0].flat)>.01, 'Frozen receiver has no unshadowed baseline'
    visibility={mode:radiance[mode]/radiance[0] for mode in (3,5)}
    curves=[]
    for j,receiver in enumerate(fixture['receivers']):
        row=dict(center=receiver['center'],offsets_degrees=offsets.tolist())
        for mode,key in ((3,'old'),(5,'new')):
            # Independent frozen build output at these same 13 directions.
            # This is a route regression oracle, not a quality/teacher oracle.
            expected=receiver[key+'_diffuse'];observed=radiance[mode][j,::3]
            assert np.allclose(observed,expected,atol=.002,rtol=.005), (key,'frozen route oracle differs')
            v=visibility[mode][j]
            assert v.min()>=-.002 and v.max()<=1.005
            step=float(np.abs(np.diff(v)).max());tv=float(np.abs(np.diff(v)).sum())
            # Bounds apply only to these fixed smooth-transition receivers.
            assert step<.15 and tv<1.2, (key,'unstable receiver transition',step,tv)
            row[key]=dict(visibility=v.tolist(),max_one_degree_step=step,total_variation=tv)
        difference=float(np.max(np.abs(visibility[3][j]-visibility[5][j])))
        assert difference>.2, 'Old and new estimators did not differ at the known receiver'
        row['max_old_new_visibility_difference']=difference;curves.append(row)
    assert max(contrast)>1000, 'Estimator difference came from outside the wall'
    for mode in (3,5):
        assert np.ptp(wall_sums[mode])>1, 'Moving-light change came from outside the wall'
    metrics['moving']=dict(wall_pixels=int(mask.sum()),max_differing_wall_pixels=max(contrast),receivers=curves,
        scope='Fixed receiver route/transition regressions; no engine teacher-ground-truth or whole-wall continuity claim')


def validate(root):
    compare(root)
    report = json.loads((root/'results.json').read_text())
    assert report['near_only'] and report['saved_level_unchanged']
    metrics = {}
    for group in ('environment','direct'):
        a,b,c = (hit(root/f'{group}-{state}-Hit.dds') for state in ('off','on','restored'))
        assert np.array_equal(a,b) and np.array_equal(a,c), 'AO changed geometry'
        mask = wall_mask(a)
        assert mask.sum()>1000
        for slot in ('DiffuseOutput','SpecularOutput','AlbedoOutput','NormalOutput'):
            off,on,restored = (attachment(root/f'{group}-{state}-{slot}.dds')[mask].astype(float)
                               for state in ('off','on','restored'))
            assert np.isfinite(on).all()
            assert np.array_equal(off,restored), (group,slot,'restore differs')
            delta = np.abs(off-on)
            metrics[f'{group}/{slot}/max_delta'] = float(delta.max())
            if slot=='AlbedoOutput':
                assert np.array_equal(off[:,3],on[:,3]), 'Specular AO changed'
                assert (on[:,:3] <= off[:,:3]).all() and np.any(on[:,:3]<off[:,:3])
            elif slot in ('SpecularOutput','NormalOutput') or group=='direct':
                assert delta.max() <= .002, (group,slot,delta.max())
    ambient(root,wall_mask(hit(root/'environment-off-Hit.dds')),metrics)
    moving(root,report,metrics)
    (root/'visual-validation.json').write_text(json.dumps(metrics,indent=2))
    print(json.dumps(metrics,indent=2))


def negative_controls(root):
    global attachment
    real_attachment=attachment
    def black(path):
        value=real_attachment(path)
        if path.name.endswith('-Indirect.dds'):value*=0
        return value
    attachment=black
    try:
        ambient(root,wall_mask(hit(root/'environment-off-Hit.dds')),dict())
    except AssertionError as error:
        assert 'nonzero indirect' in str(error)
    else:raise AssertionError('Black ambient false positive')
    finally:attachment=real_attachment
    def old_as_new(path):
        if path.name.startswith('moving-5-') and path.name.endswith('-DiffuseOutput.dds'):
            path=path.with_name(path.name.replace('moving-5-','moving-3-'))
        return real_attachment(path)
    attachment=old_as_new
    try:
        moving(root,json.loads((root/'results.json').read_text()),dict())
    except AssertionError as error:
        assert 'oracle differs' in str(error) or 'did not differ' in str(error)
    else:raise AssertionError('Old estimator routed as new false positive')
    finally:attachment=real_attachment
    print('Negative controls rejected black indirect illumination and old-as-new estimator routing.')


if __name__=='__main__':
    validate(Path(sys.argv[1]))
    if '--negative-controls' in sys.argv:negative_controls(Path(sys.argv[1]))

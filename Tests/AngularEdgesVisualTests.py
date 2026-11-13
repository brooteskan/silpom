"""Validate the isolated near-wall capture, including diffuse-only AO routing."""
import json
from pathlib import Path
import struct
import sys
import numpy as np
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


def validate(root):
    compare(root)
    report = json.loads((root/'results.json').read_text())
    assert report['near_only'] and report['saved_level_unchanged']
    metrics = {}
    for group in ('environment','direct'):
        a,b,c = (hit(root/f'{group}-{state}-Hit.dds') for state in ('off','on','restored'))
        assert np.array_equal(a,b) and np.array_equal(a,c), 'AO changed geometry'
        mask = ((a[:,3]&3)==1) & ((a[:,2]&0x40000000)!=0)
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
    direct = []
    for i in range(13):
        old,new = (hit(root/f'moving-{mode}-{i:02d}-Hit.dds') for mode in (3,5))
        assert np.array_equal(old,new), 'Estimator changed geometry'
        values = attachment(root/f'moving-5-{i:02d}-DiffuseOutput.dds')
        assert np.isfinite(values).all()
        direct.append(float(values[:,:3].astype(float).sum()))
    assert max(direct)-min(direct)>1, 'Moving light did not change illumination'
    metrics['moving_light_direct_sum_range'] = max(direct)-min(direct)
    (root/'visual-validation.json').write_text(json.dumps(metrics,indent=2))
    print(json.dumps(metrics,indent=2))


if __name__=='__main__':
    validate(Path(sys.argv[1]))

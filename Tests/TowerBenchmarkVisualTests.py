"""Reject dark/blank tower fixtures and record repeatable capture evidence."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import numpy as np
from PIL import Image


def validate(folder):
    result=json.loads((folder/'results.json').read_text())
    assert result['passed'] and result['restored'] and result['gameplay_visible']
    assert result['saved_level_unchanged'] and result['vue_live_link_unchanged']
    captures={}
    for name in ('exterior','lower-facing','lower-glancing','upper-glancing','gameplay'):
        rgba=np.array(Image.open(folder/(name+'.png')).convert('RGBA'))
        assert rgba.shape==(720,1280,4),(name,rgba.shape)
        rgb=rgba[:,:,:3]
        mean=float(rgb.mean());contrast=float(rgb.std())
        assert 5<mean<235 and contrast>10,(name,'Dark or blank comparison',mean,contrast)
        captures[name]={'rgba_sha256':hashlib.sha256(rgba.tobytes()).hexdigest(),
            'mean_rgb':round(mean,3),'contrast':round(contrast,3)}
    for name in ('lower-facing','lower-glancing','upper-glancing'):
        raw=(folder/(name+'-hit.dds')).read_bytes()
        assert raw[:4]==b'DDS ' and struct.unpack_from('<I',raw,128)[0]==3
        hits=np.frombuffer(raw,dtype='<u4',offset=148).reshape(-1,4)
        valid=int(np.count_nonzero((hits[:,3]&3)==1))
        assert valid>1280*720*.25,(name,'Tower hit coverage missing',valid)
        captures[name]['valid_hit_samples']=valid
    return {'passed':True,'scene_sha256':result['scene_sha256'],
        'width':1280,'height':720,'samples':1,'gameplay_visible':True,
        'saved_tower_controls':result['controls'],'vue_live_link_unchanged':True,
        'target_5700g_performance_measured':False,'captures':captures}


if __name__=='__main__':
    report=validate(Path(sys.argv[1]))
    if len(sys.argv)>2:Path(sys.argv[2]).write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

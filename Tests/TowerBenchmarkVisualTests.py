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
    assert result['saved_level_unchanged']
    assert result['controls'].get('Height-derived normals (off = cheaper)') is True,'Height-derived normals must remain enabled'
    samples=int(result.get('samples',1))
    assert samples in (1,2,4)
    captures={}
    for name in result.get('image_views',('exterior','lower-facing','lower-glancing','upper-glancing','gameplay')):
        rgba=np.array(Image.open(folder/(name+'.png')).convert('RGBA'))
        assert rgba.shape==(720,1280,4),(name,rgba.shape)
        rgb=rgba[:,:,:3]
        mean=float(rgb.mean());contrast=float(rgb.std())
        assert 5<mean<235 and contrast>10,(name,'Dark or blank comparison',mean,contrast)
        captures[name]={'rgba_sha256':hashlib.sha256(rgba.tobytes()).hexdigest(),
            'mean_rgb':round(mean,3),'contrast':round(contrast,3)}
    for name in result.get('record_capture_views',('lower-facing','lower-glancing','upper-glancing')):
        raw=(folder/(name+'-hit.dds')).read_bytes()
        assert raw[:4]==b'DDS ' and struct.unpack_from('<I',raw,128)[0]==3
        if result.get('hit_capture_layout')=='four-columns-per-pixel sample atlas' or samples>1:
            height,width=struct.unpack_from('<II',raw,12)
            assert (height,width)==(720,5120),(height,width)
            hits=np.frombuffer(raw,dtype='<u4',offset=148).reshape(height,1280,4,4)[:,:,:samples,:]
            depth=(folder/(name+'-depth.dds')).read_bytes()
            assert struct.unpack_from('<I',depth,128)[0]==41
            depth_bits=np.frombuffer(depth,dtype='<u4',offset=148).reshape(height,1280,4)[:,:,:samples]
            backup=((hits[...,3]&3)==1)&((hits[...,2]&0xc0000000)==0xc0000000)
            mismatches=int(np.count_nonzero(backup&((hits[...,2]&0x3fffffff)!=depth_bits)))
            captures[name]['backup_depth_mismatches']=mismatches
            captures[name]['backup_samples']=int(np.count_nonzero(backup))
            assert mismatches==0,(name,'Backup hit/depth equality failed',mismatches)
            hits=hits.reshape(-1,4)
        else:
            hits=np.frombuffer(raw,dtype='<u4',offset=148).reshape(-1,4)
        valid=int(np.count_nonzero((hits[:,3]&3)==1))
        # The wide exterior camera places the tower in a small part of the HDRI
        # view; close interior/movement cameras require substantial coverage.
        minimum_fraction=.01 if name=='exterior' else .25
        assert valid>1280*720*samples*minimum_fraction,(name,'Tower hit coverage missing',valid)
        captures[name]['valid_hit_samples']=valid
    return {'passed':True,'scene_sha256':result['scene_sha256'],
        'width':1280,'height':720,'samples':samples,'gameplay_visible':True,
        'saved_tower_controls':result['controls'],
        'requested_overrides':result.get('requested_overrides',{}),
        'saved_height_derived_normals':result.get('saved_height_derived_normals'),
        'effective_tower_configuration_sha256':result['effective_tower_configuration_sha256'],
        'target_5700g_performance_measured':False,'camera_movement_complete':result.get('camera_movement_complete',False),
        'captures':captures}


if __name__=='__main__':
    report=validate(Path(sys.argv[1]))
    if len(sys.argv)>2:Path(sys.argv[2]).write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

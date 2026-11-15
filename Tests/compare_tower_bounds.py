"""Compare independent 2x fixture runs, including exact per-sample winners/depth.

Run outside measured intervals. Integer atlas data must never be MSAA-resolved.
"""
import argparse
import json
from pathlib import Path
import struct
import numpy as np
from PIL import Image


def atlas(path, channels, format_id):
    raw=path.read_bytes()
    assert raw[:4]==b'DDS ' and struct.unpack_from('<I',raw,128)[0]==format_id
    assert struct.unpack_from('<II',raw,12)==(720,5120)
    shape=(720,1280,4,channels) if channels>1 else (720,1280,4)
    return np.frombuffer(raw,dtype='<u4',offset=148).reshape(shape)[:,:,:2]


def compare(before,after):
    metadata=[json.loads((p/'results.json').read_text()) for p in (before,after)]
    for m in metadata:
        assert m['passed'] and m['restored'] and m['samples']==2
        assert m['controls']['Height-derived normals (off = cheaper)'] is True
    for key in ('scene_sha256','effective_tower_configuration_sha256','controls','initial_aa_state'):
        assert metadata[0][key]==metadata[1][key],key
    report=dict(passed=True,before=str(before),after=str(after),views={})
    for view in ('exterior','lower-facing','lower-glancing','upper-glancing','gameplay'):
        images=[np.array(Image.open(p/(view+'.png')).convert('RGBA')) for p in (before,after)]
        delta=np.abs(images[0].astype(int)-images[1].astype(int))
        row=dict(image_changed_pixels=int(np.count_nonzero(delta.max(axis=2))),
                 image_max_channel_delta=int(delta.max()),image_mean_absolute_delta=float(delta.mean()))
        if view not in ('exterior','gameplay'):
            hits=[atlas(p/(view+'-hit.dds'),4,3) for p in (before,after)]
            depths=[atlas(p/(view+'-depth.dds'),1,41) for p in (before,after)]
            row['samples']=[]
            for s in range(2):
                a,b=hits[0][:,:,s],hits[1][:,:,s]
                valid_a,valid_b=(a[...,3]&3)==1,(b[...,3]&3)==1
                common=valid_a&valid_b
                sample=dict(sample=s,baseline_valid=int(valid_a.sum()),candidate_valid=int(valid_b.sum()),
                    lost=int(np.count_nonzero(valid_a&~valid_b)),gained=int(np.count_nonzero(~valid_a&valid_b)),
                    changed_winner_records=int(np.count_nonzero(np.any(a!=b,axis=2)&common)),
                    changed_depth_bits=int(np.count_nonzero(depths[0][:,:,s]!=depths[1][:,:,s])),
                    changed_record_components=[int(np.count_nonzero((a[...,c]!=b[...,c])&common)) for c in range(4)])
                sample['passed']=not any(sample[k] for k in ('lost','gained','changed_winner_records','changed_depth_bits'))
                row['samples'].append(sample)
                report['passed'] &= sample['passed']
        report['views'][view]=row
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('before',type=Path);parser.add_argument('after',type=Path)
    parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    report=compare(args.before,args.after);args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    raise SystemExit(0 if report['passed'] else 1)

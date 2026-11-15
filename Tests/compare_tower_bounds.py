"""Compare independent 2x fixture runs, including exact per-sample winners/depth.

Run outside measured intervals. Integer atlas data must never be MSAA-resolved.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import numpy as np
from PIL import Image


def atlas(path, channels, format_id):
    raw=path.read_bytes()
    assert raw[:4]==b'DDS ' and struct.unpack_from('<I',raw,128)[0]==format_id
    height,width=struct.unpack_from('<II',raw,12)
    assert height==720 and width in (1280,5120)
    columns=width//1280
    shape=(720,1280,columns,channels) if channels>1 else (720,1280,columns)
    return np.frombuffer(raw,dtype='<u4',offset=148).reshape(shape)


def payload_identity(arrays):
    """Evidence for the ENTIRE DDS texel payload, including unused columns."""
    a,b=arrays
    return dict(bytes=a.nbytes,baseline_sha256=hashlib.sha256(a).hexdigest(),
                candidate_sha256=hashlib.sha256(b).hexdigest(),equal=bool(np.array_equal(a,b)))


def compare(before,after):
    metadata=[json.loads((p/'results.json').read_text()) for p in (before,after)]
    for m in metadata:
        assert m['passed'] and m['restored'] and m['samples'] in (1,2,4)
        assert m['controls']['Height-derived normals (off = cheaper)'] is True
    for key in ('scene_sha256','effective_tower_configuration_sha256','controls','initial_aa_state','samples'):
        assert metadata[0][key]==metadata[1][key],key
    for key in ('image_views','record_capture_views','camera_sequence'):
        assert metadata[0].get(key)==metadata[1].get(key),key
    samples=metadata[0]['samples']
    image_views=metadata[0].get('image_views',('exterior','lower-facing','lower-glancing','upper-glancing','gameplay'))
    record_views=metadata[0].get('record_capture_views',('lower-facing','lower-glancing','upper-glancing'))
    assert set(record_views)<=set(image_views)
    report=dict(passed=True,before=str(before),after=str(after),samples=samples,
                whole_atlas_payloads_identical=True,views={})
    for view in image_views:
        images=[np.array(Image.open(p/(view+'.png')).convert('RGBA')) for p in (before,after)]
        assert images[0].shape==images[1].shape,(view,'Image dimensions differ')
        delta=np.abs(images[0].astype(int)-images[1].astype(int))
        row=dict(image_changed_pixels=int(np.count_nonzero(delta.max(axis=2))),
                 image_max_channel_delta=int(delta.max()),image_mean_absolute_delta=float(delta.mean()))
        row['image_equal']=row['image_changed_pixels']==0
        row['passed']=row['image_equal']
        if view in record_views:
            hits=[atlas(p/(view+'-hit.dds'),4,3) for p in (before,after)]
            depths=[atlas(p/(view+'-depth.dds'),1,41) for p in (before,after)]
            row['atlas_payload_identity']={label:payload_identity(arrays) for label,arrays in
                (('hit_records',hits),('depth',depths))}
            # Full payload identity is diagnostic evidence. Semantic equality
            # gates only real samples/valid winners; unused texels may be undefined.
            report['whole_atlas_payloads_identical'] &= all(v['equal'] for v in row['atlas_payload_identity'].values())
            assert all(a.shape[2]>=samples for a in hits+depths)
            row['samples']=[]
            for s in range(samples):
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
                row['passed'] &= sample['passed']
        report['passed'] &= row['passed']
        report['views'][view]=row
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('before',type=Path);parser.add_argument('after',type=Path)
    parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    report=compare(args.before,args.after);args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    raise SystemExit(0 if report['passed'] else 1)

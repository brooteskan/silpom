"""Compare exact real-sample paint outputs, outside timing runs."""
import argparse,json,struct
from pathlib import Path
import numpy as np

def load(path,format_id,dtype):
    raw=path.read_bytes()
    assert raw[:4]==b'DDS ' and struct.unpack_from('<I',raw,128)[0]==format_id,path
    h,w=struct.unpack_from('<II',raw,12)
    assert (h,w)==(720,5120),(path,h,w)
    return np.frombuffer(raw,dtype=dtype,offset=148).reshape(h,1280,4,4)[:,:,:2,:]

p=argparse.ArgumentParser();p.add_argument('folder',type=Path);args=p.parse_args()
folder=args.folder;meta=json.loads((folder/'results.json').read_text())
assert meta['passed'] and meta['saved_level_unchanged']
assert meta['height_derived_sampling'] and meta['runtime_height_derived_sampling']
assert meta['runtime_receiver_uv']['state']=='unbound'
assert meta['paint_bake_off'].lower() in ('false','0','0.0')
report={'passed':True,'samples':2,'paint_baking_off':True,'height_derived_sampling':True,'views':{}}
for view in ('outside','inside'):
    stems=[folder/f'synthetic-{view}-{r}-{m}' for r,m in ((0,0),(1,1))]
    hits=[load(Path(str(s)+'-hit.dds'),3,'<u4') for s in stems]
    valid=[(h[...,3]&3)==1 for h in hits]
    assert valid[0].sum()>10000,(view,'Fixture missing tower coverage')
    common=valid[0]&valid[1]
    row={'valid_samples':int(common.sum()),'lost':int(np.count_nonzero(valid[0]&~valid[1])),
        'gained':int(np.count_nonzero(~valid[0]&valid[1])),
        'changed_winner_records':int(np.count_nonzero(np.any(hits[0]!=hits[1],axis=-1)&common))}
    for label in ('diffuse','normal'):
        values=[load(Path(str(s)+'-'+label+'.dds'),10,'<u2') for s in stems]
        row[label+'_changed_samples']=int(np.count_nonzero(np.any(values[0]!=values[1],axis=-1)&common))
        row[label+'_real_sample_planes_equal']=bool(np.array_equal(*values))
    row['passed']=not any(row[k] for k in ('lost','gained','changed_winner_records','diffuse_changed_samples','normal_changed_samples'))
    if view=='inside':
        painted=load(Path(str(stems[1])+'-diffuse.dds'),10,'<u2')
        hidden=load(folder/'synthetic-inside-no-paint-diffuse.dds',10,'<u2')
        row['visible_paint_samples']=int(np.count_nonzero(np.any(painted!=hidden,axis=-1)&valid[1]))
        row['passed'] &= row['visible_paint_samples']>0
    report['passed'] &= row['passed'];report['views'][view]=row
(folder/'paint-validation.json').write_text(json.dumps(report,indent=2))
print(json.dumps(report,indent=2))
raise SystemExit(0 if report['passed'] else 1)

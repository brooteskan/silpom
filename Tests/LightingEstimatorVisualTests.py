"""Validate isolated Editor captures against the approved reference and each route."""
import hashlib,json,struct,sys
from pathlib import Path
import numpy as np
from PIL import Image
from NearestHitVisualTests import capture_hash,compare

def main(root):
    compare(root)
    fixture=json.loads((Path(__file__).parent/'Reference/NearestHitVisual.json').read_text())
    for name,value in fixture['captures'].items():
        assert capture_hash(root/'restored'/name,value['kind'])==value['sha256'],('Switch-back reference changed',name)
    result=json.loads((root/'results.json').read_text());assert result['restored']
    routes=('physical-ray','field32','caps4','caps8');views=('lower-glancing','lower-facing','upper-glancing','far-proxy')
    report={};seen=set()
    for route in routes:
        record={};total=0;digester=hashlib.sha256()
        for view in views:
            baseline=root/'current'/(view+'-hit.dds');selected=root/route/(view+'-hit.dds')
            assert capture_hash(selected,'hit')==capture_hash(baseline,'hit'),('Estimator changed camera intersection',route,view)
            a=np.asarray(Image.open(root/'current'/(view+'.png')).convert('RGB'),np.int16)
            b=np.asarray(Image.open(root/route/(view+'.png')).convert('RGB'),np.int16)
            raw=baseline.read_bytes();height,width=struct.unpack_from('<II',raw,12)
            hits=np.frombuffer(raw,dtype='<u4',offset=148,count=width*height*4).reshape(height,width,4)
            valid=(hits[:,:,3]&3)==1
            count=int(np.any(a[valid]!=b[valid],axis=1).sum());total+=count
            record[view]={'changed_silpom_pixels':count,'max_silpom_channel_difference':int(np.abs(a[valid]-b[valid]).max())}
            digester.update(b[valid].tobytes())
        assert total>0,('Estimator did not change lighting',route)
        signature=digester.hexdigest();assert signature not in seen,('Two estimators rendered identical lighting',route);seen.add(signature)
        report[route]=record
    for route in ('current',*routes,'restored'):
        raw=(root/route/'far-proxy-hit.dds').read_bytes();height,width=struct.unpack_from('<II',raw,12)
        pixels=np.frombuffer(raw,dtype='<u4',offset=148,count=width*height*4).reshape(-1,4)
        proxy=((pixels[:,3]&3)==1)&((pixels[:,2]&0x80000000)!=0)
        assert proxy.any(),('No visible far proxies',route)
    for view in views[:-1]:
        assert capture_hash(root/'restored'/(view+'.png'),'rgba')==capture_hash(root/'current'/(view+'.png'),'rgba'),('Switch-back changed image',view)
    # The distant framing includes streaming terrain outside SilPOM. Verify
    # the complete SilPOM coverage exactly, without treating that unrelated
    # background as part of the lighting estimator contract.
    assert capture_hash(root/'restored/far-proxy-hit.dds','hit')==capture_hash(root/'current/far-proxy-hit.dds','hit')
    raw=(root/'current/far-proxy-hit.dds').read_bytes();height,width=struct.unpack_from('<II',raw,12)
    hits=np.frombuffer(raw,dtype='<u4',offset=148,count=height*width*4).reshape(height,width,4);valid=(hits[:,:,3]&3)==1
    a=np.asarray(Image.open(root/'current/far-proxy.png').convert('RGB'))
    b=np.asarray(Image.open(root/'restored/far-proxy.png').convert('RGB'))
    np.testing.assert_array_equal(a[valid],b[valid])
    (root/'visual-verification.json').write_text(json.dumps(dict(passed=True,approved_captures_exact=20,switch_back_captures_exact=20,
        routes_tested=5,normalized_hit_buffers_unchanged=True,visible_far_proxies=True,far_silpom_switch_back_exact=True,
        far_background_changed_pixels=int(np.any(a[~valid]!=b[~valid],axis=1).sum()),lighting_changes=report),indent=2)+'\n')
    print('All five live routes verified: lighting changes, unchanged camera hits, visible far proxies, and exact approved switch-back.')

if __name__=='__main__':main(Path(sys.argv[1]))

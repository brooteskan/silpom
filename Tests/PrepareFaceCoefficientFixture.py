"""Freeze export-derived FP32 canonical faces for coefficient-cache tests.

The binary is a test input, not a live rendering-buffer readback. Keep that
distinction in proof reports; arbitrary final packed FP32 records are also
accepted by both test executables.
"""
import hashlib
import json
from pathlib import Path
import numpy as np

project=Path(__file__).resolve().parent/'Fixtures/TowerBenchmark'
source=project/'Assets/SilPOMPlanarAB/IrishFieldstoneTowerOpenShaft/tower-open-shaft-no-landings-silpom.silpom.json'
target=Path(__file__).resolve().parent/'Reference/FaceCoefficientTowerPacked.bin'
document=json.loads(source.read_text())
selected=[];records=[]
for index in np.linspace(0,len(document['faces'])-1,128,dtype=int):
    face=document['faces'][int(index)]
    a,b,c=(np.asarray(document['vertices'][str(v)],dtype=np.float32) for v in face['vertices'])
    uv=np.asarray(face['uv'],dtype=np.float32);t1=uv[1]-uv[0];t2=uv[2]-uv[0]
    area=np.float32(t1[0]*t2[1]-t1[1]*t2[0]);e1=b-a;e2=c-a
    cross=np.cross(e1,e2).astype(np.float32)
    area_squared=np.float32(np.float32(cross[0]*cross[0]+cross[1]*cross[1])+cross[2]*cross[2])
    if abs(area)<1e-10 or area_squared<1e-16:continue
    inverse=np.float32(np.float32(1)/area)
    du=(e1*t2[1]-e2*t1[1])*inverse;dv=(e2*t1[0]-e1*t2[0])*inverse
    normal=cross*np.float32(np.float32(1)/np.sqrt(area_squared))
    packed=np.zeros((9,4),dtype='<f4')
    packed[0,:3]=a;packed[1,:3]=du;packed[2,:3]=dv;packed[3,:3]=normal
    packed[4]=uv[:2].reshape(4);packed[5,:2]=uv[2]
    packed[6:9,:3]=np.asarray(face['directions'],dtype=np.float32)
    records.append(packed.tobytes());selected.append({'source_index':int(index),'stable_id':face['stable_id']})
data=b''.join(records);target.write_bytes(data)
metadata={'kind':'export-derived canonical FP32 face test fixture; not a live GPU buffer dump',
          'record_stride':144,'record_count':len(records),
          'source':str(source.relative_to(project)),'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
          'sha256':hashlib.sha256(data).hexdigest(),'packing':'FP32 scalar expression order with reciprocal multiplication; unit object scale',
          'faces':selected}
target.with_suffix('.json').write_text(json.dumps(metadata,indent=2)+'\n')
print(json.dumps({k:v for k,v in metadata.items() if k!='faces'},indent=2))

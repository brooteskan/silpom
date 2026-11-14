"""Provenance/format checks performed before a frozen encoding can be reused."""
import hashlib,math,struct
from pathlib import Path

HEADER=struct.Struct('<8s7I32s')
CONTRACTS={
    'material':(b'SILPMAT4',1,1664,1536,11,6656,6144,310),
    'appearance':(b'SILPAPP5',2,832,768,10,6656,6144,757),
}

def source_fingerprint(wall,mesh=None):
    digest=hashlib.sha256()
    for name in ('basecolor.png','normal.png','roughness.png')+(() if mesh is None else ('height.png',)):
        digest.update((Path(wall)/name).read_bytes())
    if mesh is not None:digest.update(Path(mesh).read_bytes())
    return digest.digest()

def verify_frozen_header(header,kind,expected_source):
    if header[:8]!=CONTRACTS[kind]:raise ValueError('Incompatible frozen '+kind+' bundle')
    if header[8]!=expected_source:raise ValueError('Stale frozen '+kind+' bundle: source fingerprint differs; rebuild it before training')

def read_verified_bundle(path,kind,expected_source):
    raw=Path(path).read_bytes()
    if len(raw)<HEADER.size:raise ValueError('Truncated frozen header')
    header=HEADER.unpack_from(raw);verify_frozen_header(header,kind,expected_source)
    w,h=header[2:4];pixels=0
    for _ in range(header[4]):pixels+=w*h;w,h=max(1,w//2),max(1,h//2)
    if len(raw)!=HEADER.size+header[7]*4+pixels*8:raise ValueError('Truncated or trailing frozen payload')
    weights=struct.unpack_from('<'+str(header[7])+'f',raw,HEADER.size)
    if not all(math.isfinite(v) for v in weights):raise ValueError('Nonfinite frozen coefficients')
    return raw,header

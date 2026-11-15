"""Float32 proposal/certificate exercised against independent float64 geometry.

The shader remains authoritative. This is a numerical stress test, not a claim
that CPU evaluation reproduces a particular DXIL instruction/FMA schedule.
Run with --geometry <depth-probe geometry.npz> for the captured 571 AABBs too.
"""
import argparse
import itertools
import json
from pathlib import Path
import numpy as np

TRIPLES = np.array(list(itertools.combinations(range(12), 3)))
BOX = np.array([[1,0,0,1],[-1,0,0,1],[0,1,0,1],[0,-1,0,1],[0,0,1,1],[0,0,-1,1]])


def planes_from_rows(rows):
    return np.array([*BOX, rows[3]+rows[0], rows[3]-rows[0],
                     rows[3]+rows[1], rows[3]-rows[1], rows[2], rows[3]-rows[2]])


def intersections(planes, tolerance, determinant_limit):
    a = planes[TRIPLES, :3]
    cross = np.stack([np.cross(a[:,1],a[:,2]), np.cross(a[:,2],a[:,0]),
                      np.cross(a[:,0],a[:,1])], axis=1)
    det = np.sum(a[:,0]*cross[:,0], axis=1)
    good = np.abs(det) >= determinant_limit
    tri, cross, det = TRIPLES[good], cross[good], det[good]
    q = -np.sum(planes[tri,3,None]*cross,axis=1)/det[:,None]
    allowance = tolerance*(1+np.abs(q).sum(axis=1))
    inside = np.all(q@planes[:,:3].T+planes[:,3] >= -allowance[:,None],axis=1)
    return q[inside], tri[inside]


def certificate_upper_float64(rows, bound, weights, selected_planes, error):
    """Bound the augmented expression with every operand promoted BEFORE math."""
    rows64 = np.asarray(rows, dtype=np.float64)
    bound64 = np.float64(bound)
    weights64 = np.asarray(weights, dtype=np.float64)
    planes64 = np.asarray(selected_planes, dtype=np.float64)
    error64 = np.asarray(error, dtype=np.float64)
    residual = rows64[2] - bound64*rows64[3] + weights64@planes64
    return residual[3] + np.abs(residual[:3]).sum(dtype=np.float64) + error64[2] + bound64*error64[3]


def propose(center, extent, world, vp, certificate_slacks=None):
    f = np.float32
    center, extent, world, vp = [np.asarray(v,dtype=f) for v in (center,extent,world,vp)]
    matrix = vp@world
    rows = np.c_[matrix[:,:3]*extent, matrix@np.r_[center,f(1)]].astype(f)
    magnitude = np.abs(world)@np.r_[np.abs(center)+extent,f(1)]
    error = f(2**-18)*(np.abs(vp)@magnitude)+f(1e-7)
    planes = planes_from_rows(rows).astype(f)
    planes[6:,3] += [error[3]+error[0],error[3]+error[0],error[3]+error[1],
                     error[3]+error[1],error[2],error[3]+error[2]]
    scale = np.max(np.abs(planes[6:,:3]),axis=1)
    if not np.all((scale>f(1e-12))&(scale<f(1e30))):return 1.,'degenerate'
    planes[6:] /= scale[:,None]
    q, tri = intersections(planes,f(1e-5),f(1e-6))
    if not len(q):return 1.,'empty'
    clip = np.c_[q,np.ones(len(q),dtype=f)]@rows.T
    good = clip[:,3]>error[3]+f(1e-6)
    if not good.any():return 1.,'eye'
    clip, tri = clip[good],tri[good]
    best = np.argmax(clip[:,2]/clip[:,3]); z,w=clip[best,2:]
    depth=z/w
    if not (0<=depth<1):return 1.,'near'
    a,b,c=planes[tri[best]]
    bc,ca,ab=np.cross(b[:3],c[:3]),np.cross(c[:3],a[:3]),np.cross(a[:3],b[:3])
    det=np.dot(a[:3],bc)
    projected=(z+error[2])/(w-error[3]);bias=f(1e-5)
    for _ in range(4):
        bound=f(projected+bias)
        if not (0<=bound<1):return 1.,'near-margin'
        gradient=rows[2,:3]-bound*rows[3,:3]
        weights=np.maximum(-np.array([np.dot(gradient,bc),np.dot(gradient,ca),np.dot(gradient,ab)])/det,f(0))
        residual=rows[2]-bound*rows[3]+weights[0]*a+weights[1]*b+weights[2]*c
        magnitude=np.sum(np.abs(rows[2])+bound*np.abs(rows[3])+weights[0]*np.abs(a)+weights[1]*np.abs(b)+weights[2]*np.abs(c))
        arithmetic=f(2**-17)*magnitude+f(1e-7)
        upper=residual[3]+np.abs(residual[:3]).sum()+error[2]+bound*error[3]+arithmetic
        if upper<0:
            # Re-evaluate the certificate in float64; independent of searching.
            exact_upper=certificate_upper_float64(rows,bound,weights,planes[tri[best]],error)
            assert exact_upper<0
            if certificate_slacks is not None:certificate_slacks.append(float(-exact_upper))
            return float(bound),'certified'
        bias*=f(10)
    return 1.,'certificate-fallback'


def exact_bound(center, extent, world, vp):
    center,extent,world,vp=[np.asarray(v,dtype=np.float64) for v in (center,extent,world,vp)]
    matrix=vp@world
    rows=np.c_[matrix[:,:3]*extent,matrix@np.r_[center,1]]
    planes=planes_from_rows(rows)
    scale=np.max(np.abs(planes[:,:3]),axis=1)
    if np.any(scale<1e-20):return None
    planes/=scale[:,None]
    q,_=intersections(planes,1e-9,1e-12)
    if not len(q):return None
    clip=np.c_[q,np.ones(len(q))]@rows.T
    good=clip[:,3]>1e-12
    if not good.any():return None
    return float(np.clip(np.max(clip[good,2]/clip[good,3]),0,1))


def run(geometry=None):
    rng=np.random.default_rng(5700)
    cases=[]; projection=np.array([[.7330642343,0,0,0],[0,1.3032252789,0,0],
        [0,0,.000195350658,.200039073825],[0,0,-1,0]],dtype=np.float32)
    # Interior, near-plane crossings, disjoint boxes, thin/large boxes and
    # randomized rotated/scaled/mirrored object transforms and translated views.
    for i in range(800):
        center=rng.uniform(-20,20,3).astype(np.float32)
        extent=(10**rng.uniform(-4,1.5,3)).astype(np.float32)
        world=np.eye(4,dtype=np.float32); vp=projection.copy()
        if i%3:
            rotation,_=np.linalg.qr(rng.normal(size=(3,3)))
            world[:3,:3]=(rotation@np.diag(rng.choice([-1,1],3)*10**rng.uniform(-1,1,3))).astype(np.float32)
            translation=(rng.uniform(-1,1,3)*10**rng.uniform(0,5)).astype(np.float32)
            world[:3,3]=translation
            view=np.eye(4,dtype=np.float32);view[:3,3]=-translation
            vp=projection@view
        cases.append((f'random-{i}',center,extent,world,vp))
    # Near clipping must retain 1; an offscreen eye crossing can tighten.
    cases += [('near',np.array([0,0,-.2]),np.array([.1,.1,.1]),np.eye(4),projection),
              ('offscreen-eye',np.array([0,-2,-.2]),np.array([.1,1,1]),np.eye(4),projection),
              ('empty',np.array([100,0,-1]),np.array([.1,.1,.1]),np.eye(4),projection)]
    if geometry:
        g=np.load(geometry)
        for i in np.flatnonzero(g['ones']):
            center=((g['low'][i]+g['high'][i])/2).astype(np.float32)
            extent=((g['high'][i]-g['low'][i])/2+2e-5).astype(np.float32)
            cases.append((f'capture-{i}',center,extent,np.eye(4),g['local_clip'].astype(np.float32)))
    report=[];certificate_slacks=[]
    for name,center,extent,world,vp in cases:
        # Both references receive the same float32 input values.
        center,extent,world,vp=[np.asarray(v,dtype=np.float32) for v in (center,extent,world,vp)]
        candidate,reason=propose(center,extent,world,vp,certificate_slacks)
        exact=exact_bound(center.astype(float),extent.astype(float),world,vp)
        assert 0<=candidate<=1,(name,candidate)
        if exact is not None:assert candidate>=exact-1e-10,(name,candidate,exact)
        if name=='near':assert candidate==1
        if name=='empty':assert candidate==1
        report.append(dict(case=name,bound=candidate,reference=exact,reason=reason))
    return dict(passed=True,cases=len(report),certified=sum(r['reason']=='certified' for r in report),
        captured_tightened=sum(r['case'].startswith('capture-') and r['bound']<1 for r in report),
        certificate_reference_dtype='float64',minimum_certificate_safety_slack=min(certificate_slacks),results=report)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--geometry',type=Path);parser.add_argument('--output',type=Path)
    args=parser.parse_args();result=run(args.geometry)
    if args.output:args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='results'}))

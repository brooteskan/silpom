"""Offline reconstruction and certification of the frozen upper-glancing bounds.

Requires the read-only DepthBoundProbe export, previous 2x geometry/depth probe,
and this checkout's generated material HLSL layout. Never edits a shader/asset.
Quality-zero sample eligibility assumes the standard Direct3D 2x pattern.
The certificates address the mathematical enclosure; they do not validate a
new float32 shader implementation or establish GPU performance improvements.
"""
from pathlib import Path
import argparse
import itertools
import json
import re
import struct
import numpy as np

def reconstruct(PROJECT, EXTRACT, CAPTURE, ROOT):
    state=json.loads((EXTRACT/'depth-bound-probe.json').read_text())
    exports={(b['root'],b['index']):b for b in state['buffer_exports']}
    def buffer(root,index):return (EXTRACT/exports[root,index]['file']).read_bytes()
    def variables(items):
        result={}
        for item in items:
            if 'values' in item:result[item['name']]=item['values']
            result.update(variables(item.get('members',[])))
        return result
    constants={}
    for block in state['constant_values'].values():constants.update(variables(block['variables']))
    vp=np.array(constants['ViewSrg_m_viewProjectionMatrix']).reshape(4,4)
    view=np.array(constants['ViewSrg_m_viewMatrix']).reshape(4,4)
    projection=np.array(constants['ViewSrg_m_projectionMatrix']).reshape(4,4)
    model=np.eye(4);model[:3]=np.frombuffer(buffer(9,4),dtype='<f4',offset=int(constants['ObjectSrg_m_objectId'][0])*48,count=12).reshape(3,4)
    local_clip=vp@model
    material_type,material_instance=int(constants['ObjectSrg_m_materialTypeId'][0]),int(constants['ObjectSrg_m_materialInstanceId'][0])
    material_buffer=struct.unpack_from('<i',buffer(7,0),material_type*4)[0]
    material=buffer(10,material_buffer)
    hlsl=(PROJECT/'Cache/pc/materials/types/silpommesh/silpommesh_mainpipeline_silpomhitdepthlodnearwall_dx12.hlsl').read_text()
    body=hlsl.split('struct MaterialParameters',1)[1].split('};',1)[0]
    fields=re.findall(r'\b(float[234]?|int|uint|bool)\s+(\w+)\s*;',body)
    offset=0;params={};offsets={}
    for kind,name in fields:
        count=int(kind[-1]) if kind[-1].isdigit() else 1
        values=struct.unpack_from('<'+('f' if kind.startswith('float') else 'I')*count,material,material_instance*576+offset)
        params[name]=values[0] if count==1 else values;offsets[name]=offset;offset+=4*count
    assert offset==576 and params['m_materialType']==material_type and params['m_materialInstance']==material_instance
    assert params['m_silpomUseHeightDerivedNormals']==1
    faces=np.frombuffer(buffer(10,params['m_meshBufferIndex']),dtype='<f4').reshape(-1,36).astype(np.float64)
    base=faces[:,None,:3]+faces[:,None,4:7]*(faces[:,[16,18,20],None]-faces[:,None,16,None])+faces[:,None,8:11]*(faces[:,[17,19,21],None]-faces[:,None,17,None])
    directions=faces[:,[24,25,26,28,29,30,32,33,34]].reshape(-1,3,3)
    heights=params['m_silpomScale']*(np.array([0,1])-params['m_silpomReference'])
    corners=(base[:,:,None,:]+directions[:,:,None,:]*heights[None,None,:,None]).reshape(-1,6,3)
    low=corners.min(axis=1);high=corners.max(axis=1)
    center=(low+high)/2;extent=(high-low)/2+2e-5
    clip_center=np.c_[center,np.ones(len(center))]@local_clip.T
    min_w=clip_center[:,3]-extent@np.abs(local_clip[3,:3])
    max_z=clip_center[:,2]+extent@np.abs(local_clip[2,:3])
    raw=np.divide(max_z,min_w,out=np.full(len(faces),np.nan),where=min_w>1e-6)+1e-5
    bound=np.where(min_w>1e-6,np.clip(raw,0,1),1)
    post=json.loads((CAPTURE/'probe-2x/coverage-probe.json').read_text())
    vb=(CAPTURE/'probe-2x/postvs-vertices.bin').read_bytes();stride=post['postvs']['vertexByteStride']
    positions=np.ndarray((len(vb)//stride,4),dtype='<f4',buffer=vb,strides=(stride,4)).astype(np.float64)
    tags=np.ndarray((len(positions),),dtype='<u4',buffer=vb,offset=124,strides=(stride,))
    error=np.abs(bound[tags]-positions[:,2])
    assert error.max()<1e-5,'Captured shader bounds were not reproduced within their depth bias'
    captured_bound=np.empty(len(faces))
    for i in range(len(faces)):
        values=positions[tags==i,2];assert len(values)==4 and np.ptp(values)==0
        captured_bound[i]=values[0]
    coverage=json.loads((CAPTURE/'coverage-2x/proxy-faces.json').read_text())
    active=np.array([f['sample_coverage']>0 for f in coverage]);ones=active&(captured_bound==1)
    clip_corners=np.concatenate((corners,np.ones((*corners.shape[:2],1))),axis=2)@local_clip.T
    report={'params':params,'parameter_offsets':offsets,'view_projection':vp.tolist(),'model':model.tolist(),'projection':projection.tolist(),
        'camera_position':constants['ViewSrg_m_worldPosition'],'height_range':heights.tolist(),
        'faces':len(faces),'maximum_bound_error_vs_postvs':float(error.max()),'active':int(active.sum()),'active_bound_one':int(ones.sum()),
        'nonpositive_min_w':int(np.count_nonzero(ones&(min_w<=1e-6))),
        'saturating_positive_min_w':int(np.count_nonzero(ones&(min_w>1e-6))),
        'triangle_extrema_cross_near':int(np.count_nonzero(ones&((clip_corners[:,:,3]-clip_corners[:,:,2]).min(axis=1)<=0))),
        'cases':[{'face':int(i),'min_w':float(min_w[i]),'max_z':float(max_z[i]),'unclamped_ratio':float(raw[i]) if np.isfinite(raw[i]) else None,
            'six_corner_min_w':float(clip_corners[i,:,3].min()),'six_corner_min_near_distance':float((clip_corners[i,:,3]-clip_corners[i,:,2]).min()),
            'low':low[i].tolist(),'high':high[i].tolist(),'coverage':coverage[i]['sample_coverage']} for i in np.flatnonzero(ones)]}
    (ROOT/'reconstruction.json').write_text(json.dumps(report,indent=2)+'\n')
    np.savez(ROOT/'geometry.npz',faces=faces,corners=corners,low=low,high=high,min_w=min_w,max_z=max_z,bound=captured_bound,reconstructed_bound=bound,
        clip_corners=clip_corners,active=active,ones=ones,local_clip=local_clip,view=view,projection=projection)
    return report


def solve_bounds(EXTRACT, CAPTURE, ROOT):
    g=np.load(ROOT/'geometry.npz')
    meta=json.loads((ROOT/'reconstruction.json').read_text())
    M=g['local_clip'];corners=g['corners'];active=g['active'];old=g['bound']
    lo=g['low']-2e-5;hi=g['high']+2e-5

    post=json.loads((CAPTURE/'probe-2x/coverage-probe.json').read_text())
    vb=(CAPTURE/'probe-2x/postvs-vertices.bin').read_bytes();stride=post['postvs']['vertexByteStride']
    positions=np.ndarray((len(vb)//stride,4),dtype='<f4',buffer=vb,strides=(stride,4)).astype(float)
    tags=np.ndarray((len(positions),),dtype='<u4',buffer=vb,offset=124,strides=(stride,))

    def normalized(rows):
        rows=np.asarray(rows,dtype=float);norm=np.linalg.norm(rows[:,:3],axis=1)
        return rows[norm>1e-14]/norm[norm>1e-14,None]

    frustum=normalized([M[0]-M[3],-M[0]-M[3],M[1]-M[3],-M[1]-M[3],-M[2],M[2]-M[3]])
    combinations={}
    def empty_certificate(A,b,i):
        for combo in itertools.combinations(range(len(A)),4):
            selected=np.array(combo);K=np.r_[A[selected].T,np.ones((1,4))]
            if abs(np.linalg.det(K))<1e-12:continue
            w=np.linalg.solve(K,[0,0,0,1])
            if w.min()<-1e-10:continue
            w=np.maximum(w,0);res=w@A[selected]
            lower=np.maximum(res,0)@lo[i]+np.minimum(res,0)@hi[i]
            upper=w@b[selected]
            if lower>upper+1e-9:
                return {'constraints':list(combo),'weights':w.tolist(),
                    'constraint_rows':np.c_[A[selected],-b[selected]].tolist(),
                    'minimum_box_weighted_lhs':float(lower),'weighted_rhs':float(upper),
                    'contradiction_gap':float(lower-upper)}
        return None
    def vertex_bound(planes,i):
        planes=normalized(planes);A=planes[:,:3];b=-planes[:,3]
        combos=combinations.setdefault(len(A),np.array(list(itertools.combinations(range(len(A)),3))))
        matrices=A[combos];det=np.linalg.det(matrices)
        valid=np.abs(det)>1e-11
        combos=combos[valid];matrices=matrices[valid]
        vertices=np.linalg.solve(matrices,b[combos][...,None])[...,0]
        feasible=np.all(vertices@A.T<=b+2e-7,axis=1)
        if not feasible.any():return old[i],{'empty':True,'certified':False,
            'empty_certificate':empty_certificate(A,b,i),'kept_original_bound':True}
        vertices=vertices[feasible];combos=combos[feasible]
        clip=np.c_[vertices,np.ones(len(vertices))]@M.T
        z=clip[:,2]/clip[:,3]
        best=int(z.argmax());bound=min(old[i],float(np.clip(z[best]+1e-5,0,1)))
        bound=min(1.,old[i],float(np.nextafter(np.float32(bound),np.float32(np.inf))))
        if bound>=1:return 1.,{'empty':False,'certified':True,'vertices':len(vertices)}
        # A nonnegative dual combination plus a bounded residual over the parent
        # AABB proves z-B*w <= 0 throughout the whole polytope, not only at vertices.
        c=M[2,:3]-bound*M[3,:3];constant=M[2,3]-bound*M[3,3]
        chosen=combos[best]
        weights=np.linalg.solve(A[chosen].T,c)
        weights=np.maximum(weights,0.)
        residual=c-weights@A[chosen]
        upper=float(weights@b[chosen]+constant+np.maximum(residual,0)@hi[i]+np.minimum(residual,0)@lo[i])
        roundoff=2e-12*(1+np.abs(weights@b[chosen])+abs(constant)+np.linalg.norm(c)*np.linalg.norm(hi[i]-lo[i]))
        certified=upper+roundoff<=0
        return (bound if certified else old[i]),{'empty':False,'certified':bool(certified),'vertices':len(vertices),
            'dual_upper_z_minus_bound_w':upper,'dual_roundoff_allowance':float(roundoff),
            'dual_constraint_indices':chosen.tolist(),'dual_weights':weights.tolist(),
            'dual_constraint_rows':planes[chosen].tolist()}

    def box_planes(i):
        return np.array([[1,0,0,-hi[i,0]],[-1,0,0,lo[i,0]],
            [0,1,0,-hi[i,1]],[0,-1,0,lo[i,1]],[0,0,1,-hi[i,2]],[0,0,-1,lo[i,2]]],float)

    def hull_planes(points,padding):
        planes=[];seen=set()
        for tri in itertools.combinations(range(6),3):
            a,b,c=points[list(tri)];n=np.cross(b-a,c-a);length=np.linalg.norm(n)
            if length<1e-12:continue
            n/=length;dots=points@n;d=dots[tri[0]]
            if np.all(dots<=d+1e-8):pass
            elif np.all(dots>=d-1e-8):n=-n;dots=-dots
            else:continue
            d=float(dots.max()+padding*np.abs(n).sum()+1e-9)
            key=tuple(np.round(np.r_[n,d],8))
            if key not in seen:seen.add(key);planes.append(np.r_[n,-d])
        assert len(planes)>=4
        return np.asarray(planes)

    def proxy_planes(i):
        xy=positions[tags==i,:2];low=xy.min(axis=0)-[2/1280,2/720];high=xy.max(axis=0)+[2/1280,2/720]
        return normalized([M[0]-high[0]*M[3],low[0]*M[3]-M[0],M[1]-high[1]*M[3],low[1]*M[3]-M[1]])

    methods={name:old.copy() for name in ('box_corner_ratio','box_visible','box_proxy_visible','ideal_hull_visible','padded_hull_visible')}
    details={name:{} for name in methods}
    bary=2e-6
    for i in np.flatnonzero(active):
        box=np.array(list(itertools.product(*zip(lo[i],hi[i]))))
        cp=np.c_[box,np.ones(8)]@M.T
        z=1 if cp[:,3].min()<=1e-6 else np.clip((cp[:,2]/cp[:,3]).max()+1e-5,0,1)
        methods['box_corner_ratio'][i]=min(old[i],z)
        B=box_planes(i);P=proxy_planes(i);F=frustum
        tolerance=2e-6*max(1.,np.linalg.norm(g['faces'][i,4:7]),np.linalg.norm(g['faces'][i,8:11]))*max(
            meta['params']['m_silpomValidationToleranceMultiplier'],meta['params']['m_silpomDistantValidationToleranceMultiplier'],1.)
        expanded=corners[i].reshape(3,2,3)
        expanded=(1+3*bary)*expanded-bary*expanded.sum(axis=0,keepdims=True)
        H=hull_planes(expanded.reshape(6,3),tolerance+2e-5)
        candidates={'box_visible':np.r_[B,F], 'box_proxy_visible':np.r_[B,F,P],
            'ideal_hull_visible':np.r_[B,hull_planes(corners[i],0),F,P],
            'padded_hull_visible':np.r_[B,H,F,P]}
        for name,planes in candidates.items():
            methods[name][i],details[name][int(i)]=vertex_bound(planes,i)

    inputs=EXTRACT;record_meta=json.loads((inputs/'depth-bound-probe.json').read_text())
    hits=np.stack([np.fromfile(inputs/r['file'],dtype='<u4').reshape(720,1280,4) for r in record_meta['hit_records']])
    owner=(hits[:,:,:,3]>>2).astype(np.int64)-meta['params']['m_silpomHitFaceBase']
    owned=(hits[:,:,:,3]&3)==1
    owned&=(owner>=0)&(owner<len(old))
    depth=(hits[:,:,:,2]&0x3fffffff).copy().view('<f4')
    after=np.stack([np.fromfile(CAPTURE/'probe-2x'/f'depth-after-sample-{s}.bin',dtype='<f4').reshape(720,1280,2)[:,:,0] for s in range(2)])
    assert np.array_equal(depth[owned].view('<u4'),after[owned].view('<u4'))
    report={'scope':'Offline candidate bounds only; no shader replacement, runtime modification or GPU performance measurement.',
        'reproduction':{k:meta[k] for k in ('maximum_bound_error_vs_postvs','active_bound_one','nonpositive_min_w','saturating_positive_min_w','triangle_extrema_cross_near')},
        'owned_hit_samples':int(owned.sum()),'methods':{},'faces':[]}
    for name,bounds in methods.items():
        violations=depth[owned]>bounds[owner[owned]]
        remaining=g['ones']&(bounds==1)
        report['methods'][name]={'active_bound_one_remaining':int(remaining.sum()),
            'original_57_tightened':int(np.count_nonzero(g['ones']&(bounds<1))),
            'active_tightened':int(np.count_nonzero(active&(bounds<old-1e-6))),
            'captured_hit_bound_violations':int(violations.sum()),
            'maximum_captured_hit_excess':float(np.max(np.maximum(depth[owned]-bounds[owner[owned]],0))),
            'empty_volumes':sum(v.get('empty',False) for v in details[name].values()),
            'uncertified_nonempty_fallbacks':sum(not v.get('empty',False) and not v.get('certified',False) for v in details[name].values())}
    for i in np.flatnonzero(active):
        report['faces'].append({'face':int(i),'old_bound':float(old[i]),
            **{name:float(b[i]) for name,b in methods.items()},'certificates':{name:d.get(int(i)) for name,d in details.items()}})
    (ROOT/'tight-bounds.json').write_text(json.dumps(report,indent=2)+'\n')
    np.savez(ROOT/'candidate-bounds.npz',old=old,**methods)
    return report


def sample_eligibility(CAPTURE, ROOT, include_ray=False):
    g=np.load(ROOT/'geometry.npz');bounds=np.load(ROOT/'candidate-bounds.npz')
    post=json.loads((CAPTURE/'probe-2x/coverage-probe.json').read_text())
    stride=post['postvs']['vertexByteStride'];vb=(CAPTURE/'probe-2x/postvs-vertices.bin').read_bytes()
    position=np.ndarray((len(vb)//stride,4),dtype='<f4',buffer=vb,strides=(stride,4)).astype(float)
    indices=np.fromfile(CAPTURE/'probe-2x/postvs-indices.bin',dtype='<u4')
    tags=np.ndarray((len(position),),dtype='<u4',buffer=vb,offset=124,strides=(stride,))
    fixed=np.rint(np.column_stack(((position[:,0]+1)*640,(1-position[:,1])*360))*256).astype(np.int64)
    before=np.stack([np.fromfile(CAPTURE/'probe-2x'/f'depth-before-sample-{s}.bin',dtype='<f4').reshape(720,1280,2)[:,:,0]for s in range(2)])
    after=np.stack([np.fromfile(CAPTURE/'probe-2x'/f'depth-after-sample-{s}.bin',dtype='<f4').reshape(720,1280,2)[:,:,0]for s in range(2)])
    M=g['local_clip'];inv=np.linalg.inv(M)
    methods={k:bounds[k] for k in bounds.files}
    counts={k:{'eligible_before':0,'eligible_after':0,'original_57_eligible_after':0}for k in list(methods)+(['ray_box_entry'] if include_ray else [])}
    lo=g['low']-2e-5;hi=g['high']+2e-5
    coverage=0;ray_misses=0;perface=[]

    def raster(triangle,site):
        p=fixed[triangle].copy();ab=p[1]-p[0];ac=p[2]-p[0];area=ab[0]*ac[1]-ab[1]*ac[0]
        if area==0:return None
        if area<0:p[[1,2]]=p[[2,1]]
        low=np.maximum(np.floor((p.min(axis=0)-site)/256).astype(int),0)
        high=np.minimum(np.ceil((p.max(axis=0)-site)/256).astype(int),[1279,719])
        if np.any(low>high):return None
        xs=np.arange(low[0],high[0]+1)*256+site[0];ys=np.arange(low[1],high[1]+1)*256+site[1]
        mask=np.ones((len(ys),len(xs)),bool)
        for i in range(3):
            a,b=p[i],p[(i+1)%3];dx,dy=b-a;edge=dx*(ys[:,None]-a[1])-dy*(xs[None,:]-a[0])
            mask&=(edge>0)|((edge==0)&((dy<0)or(dy==0 and dx>0)))
        return low,high,xs/256,ys/256,mask

    def entry_depth(i,xs,ys):
        xx,yy=np.meshgrid(xs/640-1,1-ys/360)
        v=np.stack((xx,yy,np.ones_like(xx),np.ones_like(xx)),axis=-1)
        n=v@inv.T;n=n[...,:3]/n[...,3,None]
        v[...,2]=0;f=v@inv.T;f=f[...,:3]/f[...,3,None]
        direction=f-n
        with np.errstate(divide='ignore',invalid='ignore'):
            a=(lo[i]-n)/direction;b=(hi[i]-n)/direction
        near=np.maximum(np.minimum(a,b).max(axis=-1),0)
        far=np.minimum(np.maximum(a,b).min(axis=-1),1)
        valid=near<=far
        p=n+direction*near[...,None];clip=np.concatenate((p,np.ones((*p.shape[:2],1))),axis=2)@M.T
        depth=np.minimum(np.clip(clip[...,2]/clip[...,3]+1e-5,0,1),bounds['old'][i])
        # A missed box keeps the old bound: this comparison does not claim geometry
        # culling or elimination of shader-side ray/box rejection.
        return np.where(valid,depth,bounds['old'][i]),valid

    for number in range(len(indices)//6):
        face_indices=indices[number*6:number*6+6];face=int(tags[face_indices[0]])
        face_counts={k:0 for k in counts}
        for triangle in (face_indices[:3],face_indices[3:]):
            for sample,site in enumerate(((192,192),(64,64))):
                item=raster(triangle,np.array(site))
                if item is None:continue
                low,high,xs,ys,mask=item
                if not mask.any():continue
                region=(sample,slice(low[1],high[1]+1),slice(low[0],high[0]+1))
                coverage+=int(mask.sum())
                if include_ray:
                    ray,valid=entry_depth(face,xs,ys);ray_misses+=int(np.count_nonzero(mask&~valid))
                for name in counts:
                    bound=ray if name=='ray_box_entry' else methods[name][face]
                    b=int(np.count_nonzero(mask&(bound>=before[region])))
                    a=int(np.count_nonzero(mask&(bound>=after[region])))
                    counts[name]['eligible_before']+=b;counts[name]['eligible_after']+=a
                    if g['ones'][face]:counts[name]['original_57_eligible_after']+=a
                    face_counts[name]+=a
        perface.append({'face':face,'after':face_counts})
    assert coverage==22733080
    assert counts['old']['eligible_before']==22680953 and counts['old']['eligible_after']==14440274
    for c in counts.values():
        c['after_reduction_vs_current_percent']=(1-c['eligible_after']/counts['old']['eligible_after'])*100
    result={'scope':'Potential sample eligibility using frozen footprints and captured depth; not a simulation of dynamic Hi-Z, helper lanes or changed rendering.',
        'sample_pattern':'Assumed standard Direct3D 2x, as in the preceding coverage investigation',
        'potential_sample_coverage':coverage,'ray_box_miss_covered_samples':ray_misses,'methods':counts}
    (ROOT/'eligibility.json').write_text(json.dumps(result,indent=2)+'\n')
    (ROOT/'face-eligibility.json').write_text(json.dumps(perface,indent=2)+'\n')
    return result


def plot_example(ROOT):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    g=np.load(ROOT/'geometry.npz');meta=json.loads((ROOT/'reconstruction.json').read_text())
    bounds=np.load(ROOT/'candidate-bounds.npz');i=308;M=g['local_clip']
    lo=g['low'][i]-2e-5;hi=g['high'][i]+2e-5
    box=np.array(list(itertools.product(*zip(lo,hi))))
    B=np.array([[1,0,0,-hi[0]],[-1,0,0,lo[0]],[0,1,0,-hi[1]],[0,-1,0,lo[1]],[0,0,1,-hi[2]],[0,0,-1,lo[2]]])
    F=np.array([M[0]-M[3],-M[0]-M[3],M[1]-M[3],-M[1]-M[3],-M[2],M[2]-M[3]])
    planes=np.r_[B,F];planes/=np.linalg.norm(planes[:,:3],axis=1)[:,None]
    combos=np.array(list(itertools.combinations(range(len(planes)),3)));A=planes[:,:3];b=-planes[:,3]
    valid=np.abs(np.linalg.det(A[combos]))>1e-11;combos=combos[valid]
    vertices=np.linalg.solve(A[combos],b[combos][...,None])[...,0]
    visible=vertices[np.all(vertices@A.T<=b+2e-7,axis=1)]
    local_view=g['view']@np.array(meta['model'])
    def project(points):
        p=np.c_[points,np.ones(len(points))];return np.c_[(p@M.T)[:,3],(p@local_view.T)[:,1]]
    def hull(points):
        points=sorted(set(map(tuple,points)))
        def cross(a,b,c):return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])
        def half(seq):
            chain=[]
            for p in seq:
                while len(chain)>1 and cross(chain[-2],chain[-1],p)<=0:chain.pop()
                chain.append(p)
            return chain
        return np.array(half(points)[:-1]+half(points[::-1])[:-1])
    full=hull(project(box));clipped=hull(project(visible));six=hull(project(g['corners'][i]))
    nearest_old=project(box)[project(box)[:,0].argmin()]
    nearest_visible=project(visible)[project(visible)[:,0].argmin()]
    fig,axes=plt.subplots(1,2,figsize=(12.5,5),layout='constrained')
    for ax in axes:
        ax.fill(*full.T,color='gray',alpha=.18,label='Full expanded AABB')
        ax.fill(*six.T,color='#3578ba',alpha=.12,label='Six displacement extrema')
        ax.fill(*clipped.T,color='#26864a',alpha=.55,label='AABB inside camera frustum')
        w=np.linspace(0,1,100);y=w/g['projection'][1,1]
        ax.plot(w,y,'k--',lw=1);ax.plot(w,-y,'k--',lw=1,label='Top/bottom view limits')
        ax.axvline(.2,color='#c74438',ls=':',label='Near plane W = 0.2 m')
        ax.scatter(*nearest_visible,color='#126231',s=35,zorder=5)
        ax.set_xlabel('View depth W (m)');ax.set_ylabel('Vertical view coordinate (m)');ax.grid(alpha=.18)
    axes[0].scatter(*nearest_old,color='#c74438',s=35,zorder=5)
    axes[0].annotate('Unclipped minimum W = %.3f'%nearest_old[0],nearest_old,xytext=(-.75,-4.7),
        arrowprops={'arrowstyle':'->','color':'#c74438'},fontsize=9)
    axes[0].set_xlim(-.85,1);axes[0].set_ylim(-7,3);axes[0].set_title('Tall face 308: offscreen volume forces the old 1.0 bound')
    axes[1].set_xlim(0,.85);axes[1].set_ylim(-.65,.65);axes[1].set_title('Visible volume: nearest W = %.3f m'%nearest_visible[0])
    axes[1].annotate('Conservative candidate bound = %.5f'%bounds['box_visible'][i],nearest_visible,
        xytext=(.32,-.57),arrowprops={'arrowstyle':'->'},fontsize=9)
    axes[1].legend(fontsize=8,loc='upper left')
    fig.suptitle('Offline enclosure analysis; unchanged geometry and shaders')
    fig.savefig(ROOT/'face-308-frustum-bound.png',dpi=180);plt.close(fig)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project',type=Path,required=True)
    parser.add_argument('--inputs',type=Path,required=True)
    parser.add_argument('--capture-data',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--ray-entry',action='store_true',help='Optional spatial AABB entry comparison; misses retain old bound')
    parser.add_argument('--plot',action='store_true',help='Optional Matplotlib explanation of face 308')
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=False)
    r=reconstruct(args.project,args.inputs,args.capture_data,args.output)
    s=solve_bounds(args.inputs,args.capture_data,args.output)
    e=sample_eligibility(args.capture_data,args.output,args.ray_entry)
    if args.plot:plot_example(args.output)
    print(json.dumps({'reproduction':s['reproduction'],'owned_hit_samples':s['owned_hit_samples'],
        'candidate_checks':s['methods'],'eligibility':e},indent=2))

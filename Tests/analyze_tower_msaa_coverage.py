"""Reconstruct captured proxy coverage at explicit 1x/standard-2x sample sites.

This CPU model is not a hardware PS-invocation simulation. It excludes in-draw
depth evolution, Hi-Z granularity and helper lanes. Quality-zero 2x is modeled
with the Direct3D standard pattern; that pattern assumption is recorded.
"""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np


def analyze(folder, output):
    state=json.loads((folder/'coverage-probe.json').read_text())
    assert state['passed'] and state['final_fatal_status']=="<Result: 'Success'>"
    samples=state['sample_count'];assert samples in (1,2)
    viewport=state['rasterizer']['viewports'][0]
    width,height=int(viewport['width']),int(viewport['height'])
    assert viewport['x']==viewport['y']==0
    assert state['rasterizer']['sampleMask']==0xffffffff
    raster=state['rasterizer']['state']
    assert raster['cullMode']=='CullMode.NoCull'
    assert raster['conservativeRasterization']=='ConservativeRaster.Disabled'
    assert raster['depthBias']==raster['slopeScaledDepthBias']==raster['forcedSampleCount']==0
    assert state['depth_texture']['msSamp']==samples
    assert samples==1 or state['depth_texture']['msQual'] in (0,0xffffffff)
    assert state['depth_texture']['format']['type']=='ResourceFormatType.D32S8'
    depth_state=state['output_merger']['depthStencilState']
    assert depth_state['depthFunction']=='CompareFunction.GreaterEqual' and depth_state['depthWrites']
    assert not depth_state['stencilEnable'] and not depth_state['depthBoundsEnable']
    assert all(state['controls'][k] is True for k in ('Height-derived normals (off = cheaper)',
        'Gather validation (may be cheaper)','R16 height textures (less GPU memory)'))
    mesh=state['postvs'];assert mesh['indexByteStride']==4 and mesh['topology']=='Topology.TriangleList'
    vb=(folder/'postvs-vertices.bin').read_bytes()
    ib=(folder/'postvs-indices.bin').read_bytes()
    positions=np.ndarray((len(vb)//mesh['vertexByteStride'],4),dtype='<f4',buffer=vb,
        strides=(mesh['vertexByteStride'],4)).astype(np.float64)
    indices=np.frombuffer(ib,dtype='<u4')
    assert len(indices)==mesh['numIndices'] and len(indices)%6==0
    assert len(positions)==len(indices)//6*4 and np.all(positions[:,3]==1)
    assert np.isfinite(positions).all() and np.all((positions[:,2]>=0)&(positions[:,2]<=1))
    tags=None;offset=0
    for parameter in state['vs_signature']:
        if parameter['semanticName']=='UV' and parameter['semanticIndex']==11:
            tags=np.ndarray((len(positions),),dtype='<u4',buffer=vb,offset=offset,
                strides=(mesh['vertexByteStride'],))
        offset+=parameter['compCount']*4
    assert offset==mesh['vertexByteStride'] and tags is not None
    fixed=np.rint(np.column_stack(((positions[:,0]+1)*width/2,
        (1-positions[:,1])*height/2))*256).astype(np.int64)
    # Index order is the standard Direct3D pattern: (+4,+4), (-4,-4)
    # sixteenths relative to the pixel center, with y increasing downward.
    sites=[(128,128)] if samples==1 else [(192,192),(64,64)]
    shape=(samples,height,width)
    coverage=np.zeros(shape,dtype=np.uint16)
    nearest_one=np.zeros_like(coverage)
    eligible_before=np.zeros_like(coverage)
    eligible_after=np.zeros_like(coverage)
    depths={}
    for label in ('before','after'):
        records=state['depth_'+label+'_samples']
        assert [r['sample'] for r in records]==list(range(samples))
        planes=[]
        for record in records:
            data=(folder/record['file']).read_bytes()
            assert len(data)==width*height*8==record['bytes']
            assert hashlib.sha256(data).hexdigest()==record['sha256']
            planes.append(np.frombuffer(data,dtype='<f4').reshape(height,width,2)[:,:,0])
        depths[label]=np.stack(planes)
        assert np.isfinite(depths[label]).all()
    assert np.all(depths['after']>=depths['before'])

    def rasterize(triangle,site):
        points=fixed[triangle].copy()
        ab,ac=points[1]-points[0],points[2]-points[0]
        area=ab[0]*ac[1]-ab[1]*ac[0]
        if area==0:return None
        if area<0:points[[1,2]]=points[[2,1]]
        lo=np.maximum(np.floor((points.min(axis=0)-site)/256).astype(int),0)
        hi=np.minimum(np.ceil((points.max(axis=0)-site)/256).astype(int),[width-1,height-1])
        if np.any(lo>hi):return None
        xs=np.arange(lo[0],hi[0]+1)*256+site[0]
        ys=np.arange(lo[1],hi[1]+1)*256+site[1]
        mask=np.ones((len(ys),len(xs)),dtype=bool)
        for i in range(3):
            a,b=points[i],points[(i+1)%3];dx,dy=b-a
            edge=dx*(ys[:,None]-a[1])-dy*(xs[None,:]-a[0])
            mask&=(edge>0)|((edge==0)&((dy<0)or(dy==0 and dx>0)))
        return lo,hi,mask

    faces=[]
    for number in range(len(indices)//6):
        face_indices=indices[number*6:number*6+6]
        unique=np.unique(face_indices);assert len(unique)==4
        face_tags=np.unique(tags[unique]);assert len(face_tags)==1
        z=positions[unique,2];assert np.ptp(z)==0
        count=before=after=0
        for triangle in (face_indices[:3],face_indices[3:]):
            for sample,site in enumerate(sites):
                item=rasterize(triangle,np.array(site))
                if item is None:continue
                lo,hi,mask=item
                region=(sample,slice(lo[1],hi[1]+1),slice(lo[0],hi[0]+1))
                coverage[region]+=mask
                if z[0]==1:nearest_one[region]+=mask
                a=mask&(z[0]>=depths['before'][region]);b=mask&(z[0]>=depths['after'][region])
                eligible_before[region]+=a;eligible_after[region]+=b
                count+=int(mask.sum());before+=int(a.sum());after+=int(b.sum())
        faces.append({'proxy':number,'authored_face':int(face_tags[0]),'depth_bound':float(z[0]),
            'sample_coverage':count,'eligible_before':before,'eligible_after':after})

    result={'scope':__doc__,'capture_sha256':state['capture_sha256'],'event':state['event'],
        'before_event':state['before_event'],'samples':samples,'width':width,'height':height,
        'sample_sites_256ths_from_pixel_corner':sites,
        'sample_pattern_note':'Pixel center at 1x; assumed standard Direct3D 2x pattern for captured msQual=0; not a queried vendor sample-location table',
        'proxies':len(faces),'vertices':len(positions),'indices':len(indices),
        'postvs_vertex_sha256':hashlib.sha256(vb).hexdigest(),'postvs_index_sha256':hashlib.sha256(ib).hexdigest(),
        'nonempty_proxies':sum(f['sample_coverage']>0 for f in faces),
        'potential_sample_coverage':int(coverage.sum()),
        'coverage_per_viewport_sample':float(coverage.sum()/(width*height*samples)),
        'max_overlap':int(coverage.max()),'overlap_p50':float(np.percentile(coverage,50)),
        'overlap_p95':float(np.percentile(coverage,95)),
        'full_viewport_proxies':sum(f['sample_coverage']>=width*height*samples*.999 for f in faces),
        'depth_one_proxies':sum(f['depth_bound']==1 and f['sample_coverage']>0 for f in faces),
        'depth_one_sample_coverage':int(nearest_one.sum()),
        'eligible_against_before_depth':int(eligible_before.sum()),
        'eligible_against_completed_depth':int(eligible_after.sum()),
        'changed_depth_samples':int(np.count_nonzero(depths['after']>depths['before'])),
        'changed_depth_pixels':int(np.count_nonzero(np.any(depths['after']>depths['before'],axis=0))),
        'per_sample':[{'sample':s,'potential_coverage':int(coverage[s].sum()),
            'eligible_before':int(eligible_before[s].sum()),'eligible_after':int(eligible_after[s].sum()),
            'changed_depth_samples':int(np.count_nonzero(depths['after'][s]>depths['before'][s]))} for s in range(samples)],
        'largest_proxies':sorted(faces,key=lambda f:f['sample_coverage'],reverse=True)[:20]}
    output.mkdir(parents=True,exist_ok=True)
    (output/'sample-coverage.json').write_text(json.dumps(result,indent=2)+'\n')
    (output/'proxy-faces.json').write_text(json.dumps(faces,indent=2)+'\n')
    np.save(output/'sample-overlap.npy',coverage)
    np.save(output/'completed-depth-eligible.npy',eligible_after)
    return result


def plot(output):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    a=np.load(output/'sample-overlap.npy').mean(axis=0)
    b=np.load(output/'completed-depth-eligible.npy').mean(axis=0)
    fig,axes=plt.subplots(1,2,figsize=(13,4.8),layout='constrained')
    for ax,values,title in zip(axes,(a,b),('Potential proxy coverage','Eligible against completed depth')):
        im=ax.imshow(values,cmap='magma',vmin=0,vmax=float(a.max()),origin='upper',interpolation='nearest')
        ax.set_title(title);ax.set_xlabel('Viewport x');ax.set_ylabel('Viewport y')
    fig.colorbar(im,ax=axes,label='Candidate proxies per sample (mean across sample planes)',shrink=.8)
    fig.suptitle('Upper-glancing proxy bounds: CPU coverage model, not measured shader invocations')
    fig.savefig(output/'sample-coverage.png',dpi=180)
    plt.close(fig)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder',type=Path);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--plot',action='store_true')
    args=parser.parse_args()
    print(json.dumps({k:v for k,v in analyze(args.folder,args.output).items()
        if k not in ('largest_proxies',)},indent=2))
    if args.plot:plot(args.output)

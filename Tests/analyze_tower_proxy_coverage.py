"""CPU raster coverage from unchanged RenderDoc VS output; no GPU or shader edits."""
from pathlib import Path
import argparse
import json
import numpy as np

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('folder',type=Path,help='Coverage probe output with post-VS and depth readbacks')
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--plot',action='store_true')
args=parser.parse_args()
folder=args.folder
root=args.output
root.mkdir(parents=True,exist_ok=True)
state=json.loads((folder/'coverage-probe.json').read_text())
assert state['passed'] and state['final_fatal_status']=="<Result: 'Success'>"
mesh=state['postvs']
assert mesh['indexByteStride']==4 and mesh['topology']=='Topology.TriangleList'
vb=(folder/'postvs-vertices.bin').read_bytes()
positions=np.ndarray((len(vb)//mesh['vertexByteStride'],4),dtype='<f4',buffer=vb,strides=(mesh['vertexByteStride'],4)).astype(np.float64)
indices=np.frombuffer((folder/'postvs-indices.bin').read_bytes(),dtype='<u4')
viewport=state['rasterizer']['viewports'][0]
width,height=int(viewport['width']),int(viewport['height'])
assert viewport['x']==viewport['y']==0
assert state['rasterizer']['state']['cullMode']=='CullMode.NoCull'
assert state['rasterizer']['state']['conservativeRasterization']=='ConservativeRaster.Disabled'
assert state['depth_texture']['msSamp']==1 and state['depth_texture']['format']['type']=='ResourceFormatType.D32S8'
assert state['output_merger']['depthStencilState']['depthFunction']=='CompareFunction.GreaterEqual'
assert state['controls']['Height-derived normals (off = cheaper)'] is True
assert state['controls']['Gather validation (may be cheaper)'] is True
assert state['controls']['R16 height textures (less GPU memory)'] is True
assert len(indices)==mesh['numIndices'] and len(indices)%6==0 and len(positions)==len(indices)//6*4
assert np.all(positions[:,3]==1) and np.all((positions[:,2]>=0)&(positions[:,2]<=1))
assert np.isfinite(positions).all() and (positions[:,3]>0).all()
# RenderDoc stream-out packs signature components in order, position first.
offset=0
face_tags=None
for parameter in state['vs_signature']:
    if parameter['semanticName']=='UV' and parameter['semanticIndex']==11:
        face_tags=np.ndarray((len(positions),),dtype='<u4',buffer=vb,offset=offset,strides=(mesh['vertexByteStride'],))
    offset+=parameter['compCount']*4
assert offset==mesh['vertexByteStride'] and face_tags is not None
ndc=positions[:,:3]/positions[:,3:4]
screen=np.column_stack(((ndc[:,0]+1)*width/2,(1-ndc[:,1])*height/2))
fixed=np.rint(screen*256).astype(np.int64)
coverage=np.zeros((height,width),dtype=np.uint16)
near_coverage=np.zeros_like(coverage)
before_depth=np.frombuffer((folder/'depth-before.bin').read_bytes(),dtype='<f4').reshape(height,width,2)[:,:,0]
after_depth=np.frombuffer((folder/'depth-after.bin').read_bytes(),dtype='<f4').reshape(height,width,2)[:,:,0]
assert np.isfinite(before_depth).all() and np.isfinite(after_depth).all()
assert (after_depth>=before_depth).all(), 'Reverse-Z depth must be monotonic for this draw'
before_eligible=np.zeros_like(coverage)
after_eligible=np.zeros_like(coverage)
faces=[]

def raster(triangle):
    points=fixed[triangle].copy()
    ab,ac=points[1]-points[0],points[2]-points[0]
    area=(ab[0]*ac[1]-ab[1]*ac[0]).item()
    if area==0:return None
    if area<0:points[[1,2]]=points[[2,1]]
    lo=np.maximum(np.floor((points.min(axis=0)-128)/256).astype(int),0)
    hi=np.minimum(np.ceil((points.max(axis=0)-128)/256).astype(int),[width-1,height-1])
    if (lo>hi).any():return None
    xs=np.arange(lo[0],hi[0]+1)*256+128
    ys=np.arange(lo[1],hi[1]+1)*256+128
    mask=np.ones((len(ys),len(xs)),dtype=bool)
    for i in range(3):
        a,b=points[i],points[(i+1)%3]
        dx,dy=b-a
        edge=dx*(ys[:,None]-a[1])-dy*(xs[None,:]-a[0])
        top_left=dy<0 or (dy==0 and dx>0)
        mask &= (edge>0)|((edge==0)&top_left)
    return lo,hi,mask

for face_number in range(len(indices)//6):
    face_indices=indices[face_number*6:face_number*6+6]
    count=0
    unique=np.unique(face_indices)
    assert len(unique)==4, 'Expected one four-vertex proxy per six indices'
    tags=np.unique(face_tags[unique])
    assert len(tags)==1, 'Each proxy must carry a single authored face index'
    depths=ndc[unique,2]
    near=bool(np.all(depths==1))
    assert np.ptp(depths)==0, 'Each face must use a constant conservative raster depth'
    eligible_before=eligible_after=0
    for tri in (face_indices[:3],face_indices[3:]):
        item=raster(tri)
        if item is None:continue
        lo,hi,mask=item
        coverage[lo[1]:hi[1]+1,lo[0]:hi[0]+1]+=mask
        if near:near_coverage[lo[1]:hi[1]+1,lo[0]:hi[0]+1]+=mask
        before=mask & (depths[0]>=before_depth[lo[1]:hi[1]+1,lo[0]:hi[0]+1])
        after=mask & (depths[0]>=after_depth[lo[1]:hi[1]+1,lo[0]:hi[0]+1])
        before_eligible[lo[1]:hi[1]+1,lo[0]:hi[0]+1]+=before
        after_eligible[lo[1]:hi[1]+1,lo[0]:hi[0]+1]+=after
        eligible_before+=int(before.sum());eligible_after+=int(after.sum())
        count+=int(mask.sum())
    xy=screen[unique]
    faces.append({'face':int(tags[0]),'proxy':face_number,'pixels':count,'viewport_equivalents':count/(width*height),
                  'depth':float(depths[0]),'depth_spread':float(np.ptp(depths)),
                  'eligible_against_before_depth':eligible_before,'eligible_against_final_depth':eligible_after,
                  'nearest_depth_one':near,'screen_min':xy.min(axis=0).tolist(),'screen_max':xy.max(axis=0).tolist()})

hist=np.bincount(coverage.reshape(-1))
data={'scope':'CPU raster-footprint reconstruction with pixel-center top-left rule and 8-bit subpixel snapping. Bound comparisons use recorded before/after depth. Does not model in-draw depth evolution, hardware Hi-Z or helper lanes.',
      'capture_sha256':state.get('capture_sha256'),'controls':state['controls'],
      'event':state['event'],'before_event':state.get('before_event',460),
      'width':width,'height':height,'proxies':len(faces),'vertices':len(positions),'triangles':len(indices)//3,
      'unique_authored_face_indices':int(len(np.unique(face_tags))),
      'nonempty_proxies':sum(f['pixels']>0 for f in faces),'zero_coverage_proxies':sum(f['pixels']==0 for f in faces),
      'sum_proxy_pixel_coverage':int(coverage.sum()),'viewport_equivalents':float(coverage.sum()/(width*height)),
      'unique_pixels_covered':int(np.count_nonzero(coverage)),'max_overlaps':int(coverage.max()),
      'overlap_p50':float(np.percentile(coverage,50)),'overlap_p95':float(np.percentile(coverage,95)),
      'full_viewport_proxies':sum(f['pixels']>=width*height*.999 for f in faces),
      'half_viewport_proxies':sum(f['pixels']>=width*height*.5 for f in faces),
      'nearest_depth_one_proxies':sum(f['nearest_depth_one'] and f['pixels']>0 for f in faces),
      'nearest_depth_one_coverage':int(near_coverage.sum()),'overlap_histogram':hist.tolist(),
      'eligible_against_before_depth':int(before_eligible.sum()),
      'eligible_against_final_depth':int(after_eligible.sum()),
      'depth_before_nonzero_pixels':int(np.count_nonzero(before_depth)),
      'depth_after_nonzero_pixels':int(np.count_nonzero(after_depth)),
      'unique_pixels_depth_changed':int(np.count_nonzero(after_depth>before_depth)),
      'depth_before_range':[float(before_depth.min()),float(before_depth.max())],
      'depth_after_range':[float(after_depth.min()),float(after_depth.max())],
      'largest_proxies':sorted(faces,key=lambda f:f['pixels'],reverse=True)[:20],
      'faces':faces}
(root/'proxy-coverage.json').write_text(json.dumps(data,indent=2)+'\n')
np.save(root/'proxy-overlap-counts.npy',coverage)
np.save(root/'proxy-near-depth-overlap-counts.npy',near_coverage)
np.save(root/'proxy-final-depth-eligible.npy',after_eligible)
print(json.dumps({k:v for k,v in data.items() if k not in ('faces','largest_proxies','overlap_histogram')},indent=2))
if args.plot:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig,axes=plt.subplots(1,2,figsize=(13,4.6),layout='constrained')
    for ax,values,title in zip(axes,(coverage,after_eligible),
            ('Proxy raster footprints','Still eligible against completed depth')):
        im=ax.imshow(values,cmap='magma',vmin=0,vmax=int(coverage.max()),origin='upper',
                     extent=(0,width,height,0),interpolation='nearest')
        ax.set_title(title)
        ax.set_xlabel('Viewport x (pixels)');ax.set_ylabel('Viewport y (pixels)')
    fig.colorbar(im,ax=axes,label='Overlapping potential fragments',shrink=.76)
    fig.suptitle('Upper-glancing hit-depth draw: captured geometry, CPU coverage reconstruction',fontsize=12)
    fig.savefig(root/'proxy-coverage.png',dpi=180)
    plt.close(fig)

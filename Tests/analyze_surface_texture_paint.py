"""Compare real surface-texture output with its independent saved-texel definition.

The oracle uses saved premultiplied source texels, actual receiving-hit UVs and
the native material normal. It does not replay stamps or fit coefficients to
rendered output. This bounded fixture uses the authored-face sidedness of the
NoMarch tower and the fixed editor camera used by surface_texture_editor_smoke.
"""
import argparse
import base64
from collections import deque
import json
from pathlib import Path
import struct

import numpy as np

from analyze_decal_bake_benchmark import dds,normals

F=np.float32
MIN_PAINTED_SAMPLES=100

# Independently retain the pinned engine's sRGB/D65 -> XYZ -> D60 -> AP1
# colorimetry. Saved paint RGB is premultiplied linear sRGB; live albedo is
# ACEScg. This linear primaries conversion commutes with premultiplication.
_XYZ_TO_AP1=np.array([[1.64102352,-.32480335,-.23642471],
    [-.66366309,1.61533189,.01675635],[.01172191,-.00828444,.98839492]],dtype=F)
_D65_TO_D60=np.array([[1.01303,.00610531,-.014971],
    [.00769823,.998165,-.00503203],[-.00284131,.00468516,.924507]],dtype=F)
_SRGB_TO_XYZ=np.array([[.41239089,.35758430,.18048084],
    [.21263906,.71516860,.07219233],[.01933082,.11919472,.95053232]],dtype=F)
_LINEAR_SRGB_TO_ACESCG=_XYZ_TO_AP1@(_D65_TO_D60@_SRGB_TO_XYZ)


def paint_color_to_acescg(rgb):
    return np.asarray(rgb,dtype=F)@_LINEAR_SRGB_TO_ACESCG.T


def decode_chart(text):
    """Independent reader for the versioned lossless source texture payload."""
    assert len(text)<=((1024*1024*8*8+22)//3)*4,'Oversized chart payload'
    data=base64.b64decode(text,validate=True)
    assert base64.b64encode(data).decode('ascii')==text,'Noncanonical chart Base64'
    assert len(data)>=20,'Truncated chart header'
    magic,width,height,count,checksum=struct.unpack_from('<5I',data)
    assert magic==0x31545053,'Unknown source texture version'
    assert 32<=width<=1024 and 32<=height<=1024 and not(width&(width-1)) and not(height&(height-1))
    assert count==width*height*8 and len(data)<=count*8+20
    raw=bytearray(count*4);offset=20;index=0
    while offset<len(data) and index<count:
        assert len(data)-offset>=4,'Truncated chart token'
        token,=struct.unpack_from('<I',data,offset);offset+=4
        length=token&0x7fffffff
        assert length and length<=count-index,'Invalid chart run'
        if token&0x80000000:
            assert len(data)-offset>=4
            chunk=data[offset:offset+4]*length;offset+=4
        else:
            assert length<=(len(data)-offset)//4
            chunk=data[offset:offset+length*4];offset+=length*4
        raw[index*4:(index+length)*4]=chunk;index+=length
    assert offset==len(data) and index==count,'Incomplete or trailing chart data'
    hashed=2166136261
    for byte in raw:hashed=((hashed^byte)*16777619)&0xffffffff
    assert hashed==checksum,'Source texture checksum mismatch'
    image=np.frombuffer(raw,dtype='<f4').reshape(height,width,8)
    assert np.all(np.isfinite(image)),'Nonfinite source paint'
    assert np.all((image[...,3]>=0)&(image[...,3]<=1)&(image[...,7]>=0)&(image[...,7]<=1))
    tolerance=F(2e-5)
    assert np.all((image[...,:3]>=-tolerance)&(image[...,:3]<=image[...,3:4]+tolerance))
    assert np.all(image[...,6]>=-tolerance)
    assert np.all(np.sum(image[...,4:7]**2,axis=-1)<=(image[...,7]+tolerance)**2)
    return image


def triangle_domain_weights(shape,triangle):
    """Classify storage texel centers against the actual receiving triangle."""
    height,width=shape
    vertices=np.asarray(triangle,dtype=F).astype(np.float64)
    assert vertices.shape==(3,2) and np.all(np.isfinite(vertices)),'Invalid receiving triangle'
    first=vertices[1]-vertices[0];second=vertices[2]-vertices[0]
    area=first[0]*second[1]-first[1]*second[0]
    assert area!=0,'Degenerate receiving triangle'
    y,x=np.mgrid[:height,:width]
    points=np.stack(((x+.5)/width,(y+.5)/height),axis=-1)
    inside=np.ones((height,width),dtype=bool)
    for first,last in ((0,1),(1,2),(2,0)):
        edge=vertices[last]-vertices[first];delta=points-vertices[first]
        side=edge[0]*delta[...,1]-edge[1]*delta[...,0]
        inside&=side>=0 if area>0 else side<=0
    assert np.any(inside),'Receiving triangle contains no source texel centers'
    return inside.astype(np.uint32)


def extend_domain_boundary(image,weights):
    """Nearest receiving center by deterministic Manhattan-distance padding.

    Padding is a sampling extension only. Its geometric weight stays zero, so
    it never contributes surface area to a later mip's average.
    """
    height,width=weights.shape;seeds=np.flatnonzero(weights.reshape(-1)>0)
    assert seeds.size,'Mip has no receiving-domain samples'
    if seeds.size==height*width:return image.copy()
    nearest=np.full(height*width,-1,dtype=np.int32);nearest[seeds]=seeds
    pending=deque(int(i) for i in seeds)
    while pending:
        index=pending.popleft();y,x=divmod(index,width)
        for neighbor in (index-1 if x else -1,index+1 if x+1<width else -1,
            index-width if y else -1,index+width if y+1<height else -1):
            if neighbor>=0 and nearest[neighbor]<0:
                nearest[neighbor]=nearest[index];pending.append(neighbor)
    return image.reshape(-1,8)[nearest].reshape(height,width,8)


def generate_mips(image,triangle=None):
    """Filter raw F32 moments, separating surface area from chart storage.

    Without a triangle this is the ordinary rectangular filter used by the
    standalone math fixtures. Real receiver charts provide their canonical UV
    triangle, and each mip averages only carried receiving-domain weights.
    """
    assert image.ndim==3 and image.shape[-1]==8
    result=[np.asarray(image,dtype=F)]
    weights=None
    if triangle is not None:
        weights=triangle_domain_weights(image.shape[:2],triangle)
        result[0]=extend_domain_boundary(result[0],weights)
    while result[-1].shape[0]>1 or result[-1].shape[1]>1:
        source=result[-1];height,width=source.shape[:2]
        y=np.minimum(np.arange(max(1,height//2))*2,height-1)
        x=np.minimum(np.arange(max(1,width//2))*2,width-1)
        yy=np.minimum(y+1,height-1);xx=np.minimum(x+1,width-1)
        if weights is None:
            low=source[y[:,None],x[None,:]]*F(.5)+source[y[:,None],xx[None,:]]*F(.5)
            high=source[yy[:,None],x[None,:]]*F(.5)+source[yy[:,None],xx[None,:]]*F(.5)
            result.append(low*F(.5)+high*F(.5))
        else:
            output=np.zeros((len(y),len(x),8),dtype=F)
            total=np.zeros((len(y),len(x)),dtype=np.uint32)
            for row,column in ((y,x),(y,xx),(yy,x),(yy,xx)):
                child=source[row[:,None],column[None,:]]
                count=weights[row[:,None],column[None,:]]
                present=count>0;first=present&(total==0);mixed=present&~first
                output[first]=child[first]
                fraction=count[mixed].astype(F)/(total[mixed]+count[mixed]).astype(F)
                output[mixed]=output[mixed]*(F(1)-fraction[:,None])+child[mixed]*fraction[:,None]
                total+=count
            weights=total
            result.append(extend_domain_boundary(output,weights))
    return result


def sample_level(image,coordinates):
    height,width=image.shape[:2];q=np.asarray(coordinates,dtype=F)
    x=np.clip(q[:,0]*F(width)-F(.5),F(0),F(width-1))
    y=np.clip(q[:,1]*F(height)-F(.5),F(0),F(height-1))
    ix=x.astype(np.int32);iy=y.astype(np.int32)
    fx=(x-ix.astype(F))[:,None];fy=(y-iy.astype(F))[:,None]
    jx=np.minimum(ix+1,width-1);jy=np.minimum(iy+1,height-1)
    low=image[iy,ix]*(F(1)-fx)+image[iy,jx]*fx
    high=image[jy,ix]*(F(1)-fx)+image[jy,jx]*fx
    return low*(F(1)-fy)+high*fy


def sample_mips(mips,coordinates,lod):
    lod=np.clip(np.asarray(lod,dtype=F),F(0),F(len(mips)-1))
    low=np.floor(lod).astype(np.int32);high=np.minimum(low+1,len(mips)-1)
    values=[]
    for levels in (low,high):
        sampled=np.empty((len(coordinates),8),dtype=F)
        for level in np.unique(levels):
            selected=levels==level;sampled[selected]=sample_level(mips[int(level)],coordinates[selected])
        values.append(sampled)
    weight=(lod-low.astype(F))[:,None]
    return values[0]*(F(1)-weight)+values[1]*weight


def neutral_normal_support(mips,coordinates,lod):
    """Exact neutrality requires zero XY in every possible hardware tap.

    An ideal weighted sum may cancel XY or give a tap zero weight while fixed
    precision GPU interpolation does not. Requiring zero source XY in all four
    bilinear taps at both selected mip levels avoids those ambiguous cases.
    """
    lod=np.clip(np.asarray(lod,dtype=F),F(0),F(len(mips)-1))
    low=np.floor(lod).astype(np.int32);high=np.minimum(low+1,len(mips)-1)
    neutral=np.ones(len(coordinates),dtype=bool)
    q=np.asarray(coordinates,dtype=F)
    for levels in (low,high):
        for level in np.unique(levels):
            selected=levels==level;image=mips[int(level)];height,width=image.shape[:2]
            x=np.clip(q[selected,0]*F(width)-F(.5),F(0),F(width-1)).astype(np.int32)
            y=np.clip(q[selected,1]*F(height)-F(.5),F(0),F(height-1)).astype(np.int32)
            xx=np.minimum(x+1,width-1);yy=np.minimum(y+1,height-1)
            clean=np.ones(int(selected.sum()),dtype=bool)
            for row,column in ((y,x),(y,xx),(yy,x),(yy,xx)):
                clean&=np.all(image[row,column,4:6]==0,axis=-1)
            neutral[selected]&=clean
    return neutral


def unit(value):
    value=np.asarray(value,dtype=F)
    length2=np.sum(value*value,axis=-1,keepdims=True)
    good=np.isfinite(length2)&(length2>F(1e-16))
    length=np.sqrt(np.where(good,length2,F(1)))
    return np.where(good,value/length,np.array([0,0,1],dtype=F))


def compose_normal(live,paint):
    """One defined shortest-arc composition; independent of historical stamps."""
    live=np.asarray(live,dtype=F);paint=np.asarray(paint,dtype=F)
    base=unit(live);coverage=np.clip(paint[:,3],F(0),F(1))
    detail=paint[:,:3].copy();detail[:,2]+=F(1)-coverage;detail=unit(detail)
    axis=np.stack((-base[:,1],base[:,0],np.zeros(len(base),dtype=F)),axis=-1)
    first=np.cross(axis,detail);second=np.cross(axis,first)
    composed=unit(detail+first+second/np.maximum(F(1)+base[:,2:3],F(1e-20)))
    opposite=base[:,2]<=F(-1+1e-5)
    composed[opposite]=unit(detail[opposite]*np.array([1,-1,-1],dtype=F))
    neutral=(paint[:,3]<=0)|((paint[:,0]==0)&(paint[:,1]==0))
    composed[neutral]=live[neutral]
    return composed


def face_frame(face,front=True):
    positions=np.array([c['position'] for c in face['corners']],dtype=F)
    uv=np.array([c['uv'] for c in face['corners']],dtype=F)
    e1=positions[1]-positions[0];e2=positions[2]-positions[0]
    a=uv[1]-uv[0];b=uv[2]-uv[0];det=a[0]*b[1]-a[1]*b[0]
    assert abs(det)>1e-10,'Degenerate source UV frame'
    du=(e1*b[1]-e2*a[1])/det;dv=(e2*a[0]-e1*b[0])/det
    plane=unit(np.cross(e1,e2));tangent=unit(du)
    handed=F(-1 if np.dot(np.cross(plane,tangent),dv)<0 else 1)
    normal=plane if front else -plane;bitangent=unit(np.cross(normal,tangent))*handed
    return np.stack((tangent,bitangent,normal),axis=0)


def expand_quad(values):
    return np.repeat(np.repeat(values,2,axis=0),2,axis=1)


def quad_derivatives(records):
    height,width=records.shape[:2]
    assert height%2==0 and width%2==0,'Capture dimensions must contain complete derivative quads'
    uv=records[...,:2].view('<f4')
    face=records[...,3]>>2;valid=(records[...,3]&3)==1
    qface=face[0::2,0::2]
    qvalid=np.ones(qface.shape,dtype=bool)
    for y,x in ((0,0),(0,1),(1,0),(1,1)):
        qvalid&=valid[y::2,x::2]&(face[y::2,x::2]==qface)&np.all(np.isfinite(uv[y::2,x::2]),axis=-1)
    # Explicit coarse derivatives: top row for dx, left column for dy.
    dx=uv[0::2,1::2]-uv[0::2,0::2]
    dy=uv[1::2,0::2]-uv[0::2,0::2]
    return uv,expand_quad(dx),expand_quad(dy),expand_quad(qvalid)


def ordered_charts(document,faces,eye):
    data=document.get('ClassData',document)
    assert data.get('TextureAuthoritative'),'Expected authoritative surface texture document'
    source=data.get('SurfacePaint',[])
    assert source,'No saved texture charts'
    receiver=source[0]['Receiver']
    assert all(chart['Receiver']==receiver for chart in source),'Oracle is bounded to one receiving instance'
    lookup={face['face']:face for face in faces};charts=[];excluded_backside=0
    for layer in data.get('Layers',[]):
        opacity=float(layer.get('Opacity',1))
        assert np.isfinite(opacity)
        if not layer.get('Visible',True) or opacity<=0:continue
        for source_chart in source:
            if source_chart['Layer']!=layer['Id']:continue
            assert source_chart['Face'] in lookup,'Saved chart face absent from source geometry'
            face=lookup[source_chart['Face']];front=source_chart.get('Front',True)
            plane=face_frame(face)[2]
            visible_front=np.dot(plane,np.asarray(eye,dtype=F)-np.asarray(face['corners'][0]['position'],dtype=F))>0
            if front!=bool(visible_front):excluded_backside+=1;continue
            image=decode_chart(source_chart['Texels']);resolution=source_chart.get('Resolution',256)
            assert image.shape==(resolution,resolution,8)
            rectangle=np.array(source_chart['Rectangle'],dtype=F)
            assert np.all(np.isfinite(rectangle)) and np.all(rectangle[2:]>rectangle[:2])
            triangle=(np.array([c['uv'] for c in face['corners']],dtype=F)-rectangle[:2])*(F(1)/(rectangle[2:]-rectangle[:2]))
            charts.append({'face':int(face['hit_face']),'frame':face_frame(face,front),
                'rectangle':rectangle,'triangle':triangle,'opacity':F(min(opacity,1)),'mips':generate_mips(image,triangle)})
    assert charts,'No visible-side source charts'
    return charts,excluded_backside


def predict(records,baseline_color,baseline_normal,charts,shader_uv=None,gradients=None):
    source_uv,dx,dy,quad_valid=quad_derivatives(records)
    # Receiving ownership is independent of the shader values under test.
    # Observed shader coordinates are used only to predict numerical sampling,
    # after their round trip has been validated against these hit records.
    sample_uv=source_uv if shader_uv is None else np.asarray(shader_uv,dtype=F)
    if gradients is not None:
        gradients=np.asarray(gradients,dtype=F);dx=gradients[...,:2];dy=gradients[...,2:4]
    face=records[...,3]>>2;receiving=(records[...,3]&3)==1
    color=np.asarray(baseline_color,dtype=F).copy();normal=np.asarray(baseline_normal,dtype=F).copy()
    domain=np.zeros(face.shape,dtype=bool);edges=np.zeros(face.shape,dtype=bool)
    normal_paint=np.zeros(face.shape,dtype=bool);color_paint=np.zeros(face.shape,dtype=bool)
    source_neutral=np.ones(face.shape,dtype=bool)
    mip_low=np.full(face.shape,-1,dtype=np.int16);mip_high=mip_low.copy()
    chart_count=np.zeros(face.shape,dtype=np.uint32)
    full_chart_count=np.zeros(face.shape,dtype=np.uint32)
    for chart in charts:
        rectangle=chart['rectangle'];inverse=F(1)/(rectangle[2:]-rectangle[:2])
        source_q=(source_uv-rectangle[:2])*inverse
        q=(sample_uv-rectangle[:2])*inverse
        in_domain=np.all((source_q>=0)&(source_q<=1)&np.isfinite(source_q),axis=-1)&receiving&(face==chart['face'])
        full_chart_count[in_domain]+=1
        qany=np.zeros(in_domain[0::2,0::2].shape,dtype=bool);qall=np.ones(qany.shape,dtype=bool)
        for y,x in ((0,0),(0,1),(1,0),(1,1)):
            qany|=in_domain[y::2,x::2];qall&=in_domain[y::2,x::2]
        edges|=expand_quad(qany&~qall)&(face==chart['face'])
        eligible=quad_valid&expand_quad(qall)&in_domain
        domain|=eligible
        chart_count[eligible]+=1
        if not np.any(eligible):continue
        extent=np.array([chart['mips'][0].shape[1],chart['mips'][0].shape[0]],dtype=F)
        a=dx[eligible]*inverse*extent;b=dy[eligible]*inverse*extent
        rho=np.maximum(np.sqrt(np.sum(a*a,axis=-1)),np.sqrt(np.sum(b*b,axis=-1)))
        lod=np.clip(np.log2(np.maximum(F(1),rho)),F(0),F(len(chart['mips'])-1))
        source_neutral[eligible]&=neutral_normal_support(chart['mips'],q[eligible],lod)
        values=sample_mips(chart['mips'],q[eligible],lod)*chart['opacity']
        color[eligible]=color[eligible]*(F(1)-values[:,3:4])+paint_color_to_acescg(values[:,:3])
        active=(values[:,7]>0)&((values[:,4]!=0)|(values[:,5]!=0))
        active_mask=np.zeros(face.shape,dtype=bool);active_mask[eligible]=active
        normal_paint|=active_mask;color_paint[eligible]|=values[:,3]>0
        if np.any(active):
            frame=chart['frame'];live=normal[active_mask]@frame.T
            detail=values[active,4:8]
            normal[active_mask]=unit(compose_normal(live,detail)@frame)
        mip_low[eligible]=np.floor(lod).astype(np.int16)
        mip_high[eligible]=np.minimum(np.floor(lod)+1,len(chart['mips'])-1).astype(np.int16)
    accepted=domain&~edges&quad_valid
    return {'color':color,'normal':normal,'accepted':accepted,'normal_paint':normal_paint&accepted,
        'source_neutral_normal':source_neutral&accepted,
        'color_paint':color_paint&accepted,'cross_face_or_invalid_quad':receiving&~quad_valid,
        'chart_edge_quad':receiving&quad_valid&edges,'mip_low':mip_low,'mip_high':mip_high,
        'chart_count':chart_count,'full_chart_count':full_chart_count}


def metrics(delta,mask):
    selected=np.asarray(delta)[mask]
    return {'pixels':int(mask.sum()),'mean':float(np.mean(selected)) if selected.size else 0.,
        'rms':float(np.sqrt(np.mean(selected*selected))) if selected.size else 0.,
        'maximum':float(np.max(selected)) if selected.size else 0.}


def compare_sample(records,baseline_color,baseline_normal,actual_color,actual_normal,charts,usage,
    baseline_packed=None,actual_packed=None,shader_uv=None,gradients=None,
    preservation_colors=None,forward_normal=None,forward_base_normal=None):
    if shader_uv is not None:validate_shader_uv(records,shader_uv,charts,usage)
    predicted=predict(records,baseline_color,baseline_normal,charts,shader_uv,gradients)
    status=(records[...,3]&3)==1;faces=records[...,3]>>2;used=usage[...,0]==3
    assert not np.any((usage[...,0]!=0)&~used),'Unexpected replay/cache usage in authoritative mode'
    assert np.all(status[used]),'Authoritative texture use outside receiving hits'
    assert np.all(usage[...,1][used]==faces[used]),'Usage record disagrees with receiving hit face'
    assert np.all(np.isin(faces[used],[c['face'] for c in charts])),'Texture use escaped the saved receiver/face domains'
    assert np.any(used),'Authoritative material was not exercised'
    assert np.array_equal(used,predicted['full_chart_count']>0),'Authoritative texture use did not match full saved chart domain'
    assert np.all(usage[...,2][used]==predicted['full_chart_count'][used]),'Usage chart count disagrees with full saved visible layers'
    assert np.all(usage[~used]==0),'Nonpainted usage record was not zero'
    mask=predicted['accepted']
    assert np.all(np.isfinite(actual_color[mask])) and np.all(np.isfinite(actual_normal[mask]))
    actual_unit=unit(actual_normal);expected_unit=unit(predicted['normal'])
    cosine=np.clip(np.sum(actual_unit*expected_unit,axis=-1),F(-1),F(1))
    degrees=np.degrees(np.arccos(cosine))
    degrees[np.all(actual_normal==predicted['normal'],axis=-1)]=0
    color_error=np.abs(np.asarray(actual_color)-predicted['color'])
    normal_metrics=metrics(degrees,predicted['normal_paint']);color_metrics=metrics(color_error,mask)
    receiving_normals=metrics(degrees,mask)
    neutral=mask&~predicted['normal_paint']
    source_neutral=predicted['source_neutral_normal']
    charted_face=np.isin(faces,[chart['face'] for chart in charts])
    uncharted=status&~charted_face
    unpainted=status&(predicted['full_chart_count']==0)
    outside_chart=unpainted&charted_face
    exact_base=baseline_packed if baseline_packed is not None else baseline_normal
    exact_actual=actual_packed if baseline_packed is not None else actual_normal
    neutral_exact=np.array_equal(exact_base[neutral],exact_actual[neutral])
    source_neutral_exact=np.array_equal(exact_base[source_neutral],exact_actual[source_neutral])
    source_neutral_raw_exact=np.array_equal(baseline_normal[source_neutral],actual_normal[source_neutral])
    uncharted_normal_exact=np.array_equal(exact_base[uncharted],exact_actual[uncharted])
    preserve_base,preserve_actual=preservation_colors if preservation_colors is not None else (baseline_color,actual_color)
    uncharted_color_exact=np.array_equal(preserve_base[uncharted],preserve_actual[uncharted])
    # A face can own a partial chart. Every receiving pixel outside all saved
    # rectangles must still retain the native result, even if it shares that
    # face or a derivative quad with painted pixels.
    unpainted_normal_exact=np.array_equal(exact_base[unpainted],exact_actual[unpainted])
    unpainted_color_exact=np.array_equal(preserve_base[unpainted],preserve_actual[unpainted])
    outside_raw_normal_exact=np.array_equal(baseline_normal[outside_chart],actual_normal[outside_chart])
    outside_raw_color_exact=np.array_equal(baseline_color[outside_chart],actual_color[outside_chart])
    mip_levels=predicted['mip_low'][mask]
    report={'receiving_samples':int(status.sum()),'authoritative_samples':int(used.sum()),
        'evaluated_samples':int(mask.sum()),'painted_normal_samples':int(predicted['normal_paint'].sum()),
        'painted_color_samples':int(predicted['color_paint'].sum()),
        'excluded_cross_face_or_invalid_quad_samples':int(predicted['cross_face_or_invalid_quad'].sum()),
        'excluded_chart_edge_quad_samples':int(predicted['chart_edge_quad'].sum()),
        'neutral_normal_samples':int(neutral.sum()),'neutral_normals_bit_exact':bool(neutral_exact),
        'source_neutral_normal_samples':int(source_neutral.sum()),
        'source_neutral_raw_normals_bit_exact':bool(source_neutral_raw_exact),
        'source_neutral_packed_normals_bit_exact':bool(source_neutral_exact),
        'source_neutral_normals_bit_exact':bool(source_neutral_exact and source_neutral_raw_exact),
        'uncharted_receiving_samples':int(uncharted.sum()),
        'uncharted_material_bit_exact':bool(uncharted_color_exact and uncharted_normal_exact),
        'unpainted_receiving_samples':int(unpainted.sum()),
        'unpainted_material_bit_exact':bool(unpainted_color_exact and unpainted_normal_exact),
        'outside_chart_same_face_samples':int(outside_chart.sum()),
        'outside_chart_raw_material_bit_exact':bool(outside_raw_color_exact and outside_raw_normal_exact),
        'color_error':color_metrics,'normal_degrees':normal_metrics,'receiving_normal_degrees':receiving_normals,
        'mip_low_min':int(mip_levels.min()) if mip_levels.size else None,
        'mip_low_max':int(mip_levels.max()) if mip_levels.size else None}
    report['quality_passed']=bool(source_neutral_exact and source_neutral_raw_exact and unpainted_color_exact and unpainted_normal_exact
        and outside_raw_color_exact and outside_raw_normal_exact
        and normal_metrics['pixels']>=MIN_PAINTED_SAMPLES
        and color_metrics['pixels']>=MIN_PAINTED_SAMPLES
        and color_metrics['rms']<=.01 and color_metrics['maximum']<=.05
        and normal_metrics['rms']<=1 and normal_metrics['maximum']<=5
        and receiving_normals['rms']<=1 and receiving_normals['maximum']<=5)
    def normal_delta(before,after):
        cosine=np.clip(np.sum(unit(before)*unit(after),axis=-1),F(-1),F(1))
        angle=np.degrees(np.arccos(cosine));angle[np.all(before==after,axis=-1)]=0
        return metrics(angle,mask)
    if forward_normal is not None:
        report['forward_normal_degrees']=normal_delta(forward_normal,predicted['normal'])
        report['raw_to_forward_normal_degrees']=normal_delta(forward_normal,actual_normal)
    if forward_base_normal is not None:
        report['base_material_normal_degrees']=normal_delta(forward_base_normal,baseline_normal)
    for field in ('forward_normal_degrees','raw_to_forward_normal_degrees','base_material_normal_degrees'):
        if field in report:
            report['quality_passed']&=report[field]['rms']<=1 and report[field]['maximum']<=5
    return report


def capture_image(root,stem,label,format,sample,channels=3):
    height,width,actual,data=dds(root/(stem+'-'+label+'.dds'))
    assert actual==format and width%4==0,(stem,label,width,actual)
    if format==3:return np.frombuffer(data,dtype='<u4').reshape(height,width,4)[:,sample::4]
    if format==41:return np.frombuffer(data,dtype='<u4').reshape(height,width)[:,sample::4]
    assert format in (2,10)
    dtype='<f4' if format==2 else '<f2'
    return np.frombuffer(data,dtype=dtype).reshape(height,width,4)[:,sample::4,:channels].astype(F)


def validate_shader_uv(records,shader_uv,charts,usage):
    source_uv=records[...,:2].view('<f4')
    # Do not let missing paint usage or an incorrect shader coordinate choose
    # which receiving addresses are checked. Raw diagnostics are available on
    # the entire face of every chart, including outside partial rectangles.
    faces=records[...,3]>>2;receiving=(records[...,3]&3)==1
    checked=receiving&np.isin(faces,[chart['face'] for chart in charts])
    assert np.all(np.isfinite(source_uv[checked])),'Nonfinite receiving hit UV'
    assert np.all(np.isfinite(np.asarray(shader_uv)[checked])),'Nonfinite shader source UV'
    error=np.abs(np.asarray(shader_uv,dtype=F)-source_uv)
    maximum=float(error[checked].max()) if np.any(checked) else 0.
    maximum_texel=0.
    for chart in charts:
        selected=checked&(faces==chart['face'])
        if not np.any(selected):continue
        rectangle=chart['rectangle'];inverse=F(1)/(rectangle[2:]-rectangle[:2])
        extent=np.array([chart['mips'][0].shape[1],chart['mips'][0].shape[0]],dtype=F)
        maximum_texel=max(maximum_texel,float((error[selected]*inverse*extent).max()))
    # The material recovers canonical UV after applying its height tiling and
    # offset. Bound that documented float32 round trip in UV and texel units;
    # the shader coordinates are observations, never an output-error fit.
    assert maximum<=1e-5 and maximum_texel<=.01,(maximum,maximum_texel,'Shader UV changed receiving address')
    return {'checked_receiving_samples':int(checked.sum()),'maximum_absolute':maximum,'maximum_chart_texel':maximum_texel}


def packed_normal(rgb):
    channels=np.rint(rgb*F(1023)).astype(np.uint32)
    return channels[...,0]|(channels[...,1]<<10)|(channels[...,2]<<20)


def identity_checks(root,sample):
    records=capture_image(root,'baseline','records',3,sample);depth=capture_image(root,'baseline','depth',41,sample)
    baseline_color=capture_image(root,'baseline','diffuse',10,sample)
    baseline_normal=packed_normal(capture_image(root,'baseline','normal',10,sample))
    receiving=(records[...,3]&3)==1;assert np.any(receiving)
    for stem in ('flat','painted','cleared','restored'):
        assert np.array_equal(records,capture_image(root,stem,'records',3,sample)),stem+' changed receiving hit records'
        assert np.array_equal(depth,capture_image(root,stem,'depth',41,sample)),stem+' changed depth'
    flat_normal=packed_normal(capture_image(root,'flat','normal',10,sample))
    assert np.array_equal(baseline_normal[receiving],flat_normal[receiving]),'Flat detail changed live normal'
    assert np.array_equal(baseline_color[receiving],capture_image(root,'cleared','diffuse',10,sample)[receiving]),'Cleared paint retained color'
    assert np.array_equal(baseline_normal[receiving],packed_normal(capture_image(root,'cleared','normal',10,sample))[receiving]),'Cleared paint retained normal detail'
    for label in ('diffuse','normal'):
        assert np.array_equal(capture_image(root,'painted',label,10,sample)[receiving],
            capture_image(root,'restored',label,10,sample)[receiving]),'Restored texture output changed '+label
    return {'hit_records_and_depth_bit_exact':True,'flat_normals_bit_exact':True,
        'cleared_material_bit_exact':True,'restored_paint_bit_exact':True}


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('output',type=Path)
    root=parser.parse_args().output
    state=json.loads((root/'results.json').read_text())
    assert state['passed'] and state['saved_level_unchanged'] and state['heightmaps_unchanged'],state
    assert state['no_skipped_resource_bindings'] and state.get('other_paint_documents_restored',True),state
    assert state['retained_dabs']==0 and state['texels_survive_json_roundtrip'],state
    assert state.get('raw_surface_diagnostics') and state.get('texture_snapshot_undo_redo_exact'),state
    assert state.get('mesh_settings_restored') and state.get('material_scope','').startswith('base SiLPOM material'),state
    assert state.get('partial_chart_fixture'),'Expected a real partial-chart preservation fixture'
    document=json.loads((root/'paint-document.json').read_text())
    selected=state['receiver'];p=np.array([c['position'] for c in selected['corners']],dtype=F)
    eye=p.mean(axis=0)+face_frame(selected)[2]*F(.55)
    charts,backside=ordered_charts(document,state['faces'],eye)
    reports=[]
    for sample in (0,1):
        identities=identity_checks(root,sample)
        records=capture_image(root,'baseline','records',3,sample)
        base_lit=capture_image(root,'baseline','diffuse',10,sample)
        actual_lit=capture_image(root,'painted','diffuse',10,sample)
        base_audit=capture_image(root,'painted','base-color',2,sample,4)
        paint_audit=capture_image(root,'painted','paint-color',2,sample,4)
        base_color=base_audit[...,:3];actual_color=paint_audit[...,:3]
        shader_uv=np.stack((base_audit[...,3],paint_audit[...,3]),axis=-1)
        base_normal=capture_image(root,'painted','base-normal',2,sample)
        actual_normal=capture_image(root,'painted','paint-normal',2,sample)
        gradients=capture_image(root,'painted','gradients',2,sample,4)
        base_packed=packed_normal(capture_image(root,'baseline','normal',10,sample))
        actual_packed=packed_normal(capture_image(root,'painted','normal',10,sample))
        usage=capture_image(root,'painted','usage',3,sample)
        address_check=validate_shader_uv(records,shader_uv,charts,usage)
        report=compare_sample(records,base_color,base_normal,actual_color,actual_normal,charts,usage,
            base_packed,actual_packed,shader_uv,gradients,preservation_colors=(base_lit,actual_lit),
            forward_normal=normals(actual_packed),forward_base_normal=normals(base_packed))
        assert report['outside_chart_same_face_samples']>=100,'Partial chart did not exercise unpainted pixels on its receiving face'
        reports.append({'sample':sample,**identities,'shader_uv_roundtrip':address_check,**report})
    result={'source':'saved source texels + native material normal; no projected-history fit',
        'capture_state':state,'samples':reports,'visible_source_charts':len(charts),
        'excluded_backside_source_charts':backside,'derivatives':'raw production shader ddx/ddy per actual MSAA sample; cross-face and invalid quads excluded',
        'color_source':'raw pre-lighting material albedo; lit DiffuseSamples retained only for unchanged-output checks',
        'minimum_painted_samples_per_msaa_sample':MIN_PAINTED_SAMPLES,
        'minimum_same_face_outside_chart_samples_per_msaa_sample':100,
        'color_limits':{'rms':.01,'maximum':.05},'normal_degree_limits':{'rms':1,'maximum':5},
        'quality_passed':all(report['quality_passed'] for report in reports)}
    (root/'analysis.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='capture_state'},indent=2))
    raise SystemExit(0 if result['quality_passed'] else 1)


if __name__=='__main__':main()

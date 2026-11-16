"""Authoritative surface textures: real editor authoring, persistence and GPU output.

Run in a dedicated rebuilt Editor --runpython session. This never saves the
level or enters gameplay. No old projected normal history is the oracle: flat
detail must preserve the live normal; paint must persist as texels; clearing
must reveal the original material; restoring those texels must restore output.
"""
import array
import base64
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import time
import traceback

import azlmbr.atom as atom
import azlmbr.bus as bus
import azlmbr.components as components
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.math as azmath
import azlmbr.paths as paths
import azlmbr.silpom as silpom
from PySide6 import QtCore

PROJECT=Path(paths.projectroot)
LEVEL=PROJECT/'Levels/VueLiveLink/VueLiveLink.prefab'
OUTPUT=Path(os.environ.get('TG_SILPOM_ESTIMATOR_OUTPUT',str(PROJECT/'user/SurfaceTexturePaintSmoke')))
OUTPUT.mkdir(parents=True,exist_ok=True)
result={'passed':False,'stage':'startup','benchmark_performed':False,'entered_gameplay':False}
hidden_documents=[]
mesh_component=None
mesh_properties={}
mesh_originals={}

MATERIAL_A='Level 4A wall material (experimental)'
MATERIAL_B='Level 4B shared material / visibility (experimental)'
PROXY='Level 5A learned relief LOD (experimental)'
PROXY_DEPTH='Level 5B bounded proxy depth (experimental)'
SHADOW='Local shadow method'
LIGHTING='Lighting estimator'
DIRECTIONAL='Neural directional visibility (off = cheaper)'
PROXY_DIRECTIONAL='Learned proxy directional response (off = cheaper)'
DIFFUSE='Diffuse ambient accessibility (experimental)'


def stage(name):
    result['stage']=name
    (OUTPUT/'results.json').write_text(json.dumps(result,indent=2)+'\n')


def wait(predicate,seconds=120):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        if predicate():return
        general.idle_wait_frames(1)
    raise TimeoutError(result['stage'])


def find(name):
    query=entity.SearchFilter();query.names=[name]
    found=entity.SearchBus(bus.Broadcast,'SearchEntities',query)
    assert found,name
    return found[0]


def get_document():
    raw=silpom.DecalPaintRequestBus(bus.Event,'GetDocumentJson',canvas)
    assert raw,'Paint document unavailable'
    return json.loads(raw)


def mesh_value(label):
    response=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentProperty',mesh_component,mesh_properties[label])
    assert response.IsSuccess(),label
    return response.GetValue()


def set_mesh_value(label,value):
    response=editor.EditorComponentAPIBus(bus.Broadcast,'SetComponentProperty',mesh_component,mesh_properties[label],value)
    assert response.IsSuccess(),(label,str(response.GetError()))
    assert mesh_value(label)==value,label


def use_bounded_base_material():
    global mesh_component
    types=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['SilPOM Mesh'],entity.EntityType().Game)
    response=editor.EditorComponentAPIBus(bus.Broadcast,'GetComponentOfType',tower,types[0]);assert response.IsSuccess()
    mesh_component=response.GetValue()
    available=editor.EditorComponentAPIBus(bus.Broadcast,'BuildComponentPropertyList',mesh_component)
    labels=(SHADOW,MATERIAL_A,MATERIAL_B,PROXY,PROXY_DEPTH,LIGHTING,DIRECTIONAL,PROXY_DIRECTIONAL,DIFFUSE)
    mesh_properties.update({label:next(p for p in available if p.endswith('|'+label)) for label in labels})
    mesh_originals.update({label:mesh_value(label) for label in labels})
    result['mesh_settings_before']=mesh_originals.copy()
    # The raw material diagnostic uses the base material evaluation variant.
    # Keep the whole proof on that same supported material path; do not compare
    # a base diagnostic against a learned/shared/proxy forward shader.
    for label in (PROXY_DEPTH,PROXY,MATERIAL_A,MATERIAL_B):
        set_mesh_value(label,False)
    # Editor property notifications are deferred; let them mark/rebuild the
    # controller before asking readiness for the requested configuration.
    general.idle_wait_frames(30)
    wait(lambda:silpom.SilPomMeshRequestBus(bus.Event,'IsReady',tower))
    assert not any(mesh_value(label) for label in (MATERIAL_A,MATERIAL_B,PROXY,PROXY_DEPTH))
    assert mesh_value(LIGHTING)==0
    result['material_scope']='base SiLPOM material; 4A/4B/5A/5B temporarily disabled through Editor API'
    result['mesh_settings_during']={label:mesh_value(label) for label in labels}


def put(document):
    error=silpom.DecalPainterBus(bus.Broadcast,'SetPaintDocument',canvas,json.dumps(document))
    assert not error,error
    general.idle_wait_frames(8)


def retained_dabs(document):
    return sum(len(l.get('Composite',[]))+sum(len(s.get('Points',[])) for s in l.get('Strokes',[]))
        for l in document['ClassData'].get('Layers',[]))


def source_pixels(chart):
    data=base64.b64decode(chart['Texels'],validate=True)
    magic,width,height,count,checksum=struct.unpack_from('<5I',data)
    assert magic==0x31545053 and count==width*height*8
    words=array.array('I');offset=20
    while offset<len(data):
        token,=struct.unpack_from('<I',data,offset);offset+=4
        length=token&0x7fffffff
        assert length and len(words)+length<=count
        if token&0x80000000:
            value,=struct.unpack_from('<I',data,offset);offset+=4
            words.extend(array.array('I',[value])*length)
        else:
            words.frombytes(data[offset:offset+length*4]);offset+=length*4
    assert offset==len(data) and len(words)==count
    values=array.array('f');values.frombytes(words.tobytes())
    assert all(math.isfinite(v) for v in values)
    return width,height,values


def coverage(document):
    color=normal=0.
    for chart in document['ClassData'].get('SurfacePaint',[]):
        width,height,values=source_pixels(chart)
        color+=sum(values[3::8]);normal+=sum(values[7::8])
    return color,normal


def stamp(material,diameter=.45,opacity=.8,normal_opacity=0.,rotation=0.,erase=False,offset=(0.,0.)):
    error=silpom.DecalPainterBus(bus.Broadcast,'ApplySurfaceTextureStamp',canvas,tower,face['face'],
        azmath.Vector2(float(uv[0]+offset[0]),float(uv[1]+offset[1])),
        azmath.Vector4(float(diameter),float(opacity),float(normal_opacity),float(rotation)),
        azmath.Color_CreateFromVector3AndFloat(azmath.Vector3(.85,.15,.1),1.),material,erase,0)
    assert not error,error
    document=get_document();assert retained_dabs(document)==0
    assert silpom.DecalPaintRequestBus(bus.Event,'GetDabCount',canvas)==0
    assert silpom.DecalPaintRequestBus(bus.Event,'GetCompositeDabCount',canvas)==0
    general.idle_wait_frames(8)
    return document


def render_ready():
    status=json.loads(silpom.SilPomMeshRequestBus(bus.Event,'GetSurfacePaintStatus',tower))
    result['surface_paint_status']=status
    return status.get('ready',False) and status.get('mode')==3 and status.get('charts',0)>0


def capture(path,pass_name,slot):
    done=[];handler=atom.FrameCaptureNotificationBusHandler()
    hierarchy=['Root','MainPipeline_0','DepthPrePass' if pass_name=='SilPOMInspectFinal' else 'OpaquePass',pass_name]
    response=atom.FrameCaptureRequestBus(bus.Broadcast,'CapturePassAttachment',str(path),hierarchy,slot,1)
    assert response.IsSuccess(),(pass_name,slot,str(response.GetError()))
    handler.connect(response.GetValue());handler.add_callback('OnFrameCaptureFinished',lambda a:done.append(a[0]))
    try:
        wait(lambda:bool(done),60)
        assert done==[atom.FrameCaptureResult_Success],(path,done)
    finally:handler.disconnect()


def evidence(name):
    current=json.loads(silpom.SilPomMeshRequestBus(bus.Event,'GetPaintFacesJson',tower))
    assert {f['face']:f['hit_face'] for f in current}=={f['face']:f['hit_face'] for f in faces},'Receiving generation changed after baseline'
    general.run_console('r_silpomMsaaInspect 1');general.idle_wait_frames(45)
    for pass_name,slot,label in [('SilPOMInspectFinal','Records','records'),('SilPOMInspectFinal','SampleDepth','depth'),
        ('SilPOMInspectUvUsage','Records','usage'),('SilPOMInspectColors','DiffuseSamples','diffuse'),
        ('SilPOMInspectColors','NormalSamples','normal')]:
        capture(OUTPUT/(name+'-'+label+'.dds'),pass_name,slot)
    if name=='painted':
        for slot,label in [('BaseColor','base-color'),('PaintColor','paint-color'),('BaseNormal','base-normal'),
            ('PaintNormal','paint-normal'),('Gradients','gradients')]:
            capture(OUTPUT/(name+'-'+label+'.dds'),'SilPOMInspectUvUsage',slot)
        result['raw_surface_diagnostics']=True


def dds(stem,label,format):
    path=OUTPUT/(stem+'-'+label+'.dds');data=path.read_bytes()
    assert data[:4]==b'DDS ' and data[84:88]==b'DX10',path
    height,width=struct.unpack_from('<II',data,12)
    assert struct.unpack_from('<I',data,128)[0]==format,path
    assert width==1280*4 and height==720,(path,width,height)
    return data[148:]


def compare(before,after,neutral=False,identical=False,painted=False):
    records=dds(before,'records',3);other=dds(after,'records',3)
    assert records==other,'Painting changed receiving hit records'
    assert dds(before,'depth',41)==dds(after,'depth',41),'Painting changed depth'
    hit=memoryview(records).cast('I')
    colors=[memoryview(dds(name,'diffuse',10)).cast('Q') for name in (before,after)]
    normals=[memoryview(dds(name,'normal',10)).cast('Q') for name in (before,after)]
    mask=(1<<48)-1;color_changes=normal_changes=receiving=0
    for row in range(720):
        for column in range(1280):
            for sample in (0,1):
                pixel=row*(1280*4)+column*4+sample
                if (hit[pixel*4+3]&3)!=1:continue
                receiving+=1
                color_changes+=(colors[0][pixel]&mask)!=(colors[1][pixel]&mask)
                normal_changes+=(normals[0][pixel]&mask)!=(normals[1][pixel]&mask)
    assert receiving>100,'Capture has no receiving surface'
    if neutral:assert normal_changes==0,('Flat detail changed live normals',normal_changes)
    if identical:assert color_changes==0 and normal_changes==0,('Persistent texture output changed',color_changes,normal_changes)
    if painted:assert color_changes>10,('Paint made no visible color contribution',color_changes)
    return {'receiving_samples':receiving,'color_changes':color_changes,'normal_changes':normal_changes,
        'hit_records_bit_exact':True,'depth_bit_exact':True}


try:
    original=LEVEL.read_bytes();result['level_sha256']=hashlib.sha256(original).hexdigest()
    heights={p:hashlib.sha256(p.read_bytes()).hexdigest() for p in (PROJECT/'Assets/SilPOMPlanarAB').rglob('height.png')}
    assert heights
    general.idle_enable(True)
    for command in ('ed_keepEditorActive 1','ed_backgroundUpdatePeriod -1','r_displayInfo 0',
        'r_silpomPaintBake 0','r_silpomMsaaInspect 0','r_silpomAntiAliasing 4'):
        general.run_console(command)
    stage('load unchanged source level')
    assert general.open_level(str(LEVEL));general.idle_wait_frames(120)
    tower=find('Irish Fieldstone Tower (Open Shaft)')
    wait(lambda:silpom.SilPomMeshRequestBus(bus.Event,'IsReady',tower))
    use_bounded_base_material()
    faces=json.loads(silpom.SilPomMeshRequestBus(bus.Event,'GetPaintFacesJson',tower));assert faces
    result['faces']=faces
    origin=components.TransformBus(bus.Event,'GetWorldTranslation',tower)
    candidates=[]
    for candidate in faces:
        p=[c['position'] for c in candidate['corners']]
        a=[p[1][i]-p[0][i] for i in range(3)];b=[p[2][i]-p[0][i] for i in range(3)]
        cross=(a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])
        area=.5*math.sqrt(sum(v*v for v in cross))
        if area<=.5:continue
        normal=tuple(v/(area*2) for v in cross);center=tuple(sum(q[i] for q in p)/3 for i in range(3))
        outward=normal[0]*(center[0]-origin.x)+normal[1]*(center[1]-origin.y)
        if candidate['profile']==1 and abs(normal[2])<.05 and outward>0:
            candidates.append((abs(center[2]-origin.z-8),candidate,center,normal))
    assert candidates
    _,face,center,normal=min(candidates,key=lambda item:item[0])
    uv=tuple(sum(c['uv'][i] for c in face['corners'])/3 for i in range(2))
    result['receiver']=face
    eye=tuple(center[i]+normal[i]*.55 for i in range(3));direction=tuple(-v for v in normal)
    rotation=(math.degrees(math.atan2(direction[2],math.hypot(*direction[:2]))),0.,
        (math.degrees(math.atan2(direction[1],direction[0]))-90+180)%360-180)
    general.set_current_view_position(*eye);general.set_current_view_rotation(*rotation)
    general.set_viewport_expansion_policy('FixedSize');general.set_viewport_size(1280,720)
    canvas=silpom.DecalPainterBus(bus.Broadcast,'CreatePaintEntity');assert canvas.IsValid()
    editor.EditorEntityAPIBus(bus.Event,'SetName',canvas,'Surface Texture Paint Smoke')
    # Legacy native projections render after the material and could completely
    # obscure this test. Hide other paint layers temporarily, restoring their
    # exact documents before exit; the source level is never saved.
    types=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['Surface Decal Paint'],entity.EntityType().Game)
    assert types,'Surface Decal Paint editor component type unavailable'
    query=entity.SearchFilter();query.components={types[0]:{}}
    for other in entity.SearchBus(bus.Broadcast,'SearchEntities',query):
        if other==canvas:continue
        raw=silpom.DecalPaintRequestBus(bus.Event,'GetDocumentJson',other)
        if not raw:continue
        saved=json.loads(raw);hidden_documents.append((other,saved))
        muted=copy.deepcopy(saved)
        for layer in muted['ClassData'].get('Layers',[]):layer['Visible']=False
        error=silpom.DecalPainterBus(bus.Broadcast,'SetPaintDocument',other,json.dumps(muted))
        assert not error,error
    result['other_paint_documents_temporarily_hidden']=len(hidden_documents)
    editor.ToolsApplicationRequestBus(bus.Broadcast,'SetSelectedEntities',[])
    empty=get_document();assert empty['ClassData'].get('TextureAuthoritative')
    assert not empty['ClassData'].get('SurfacePaint') and retained_dabs(empty)==0
    general.idle_wait_frames(60)
    wait(lambda:silpom.SilPomMeshRequestBus(bus.Event,'IsReady',tower))
    # Canonical face keys survive a rebuild, global hit indices do not. Publish
    # the exact generation which will be captured, after all editor updates.
    selected_key=face['face']
    faces=json.loads(silpom.SilPomMeshRequestBus(bus.Event,'GetPaintFacesJson',tower))
    face=next(f for f in faces if f['face']==selected_key)
    result['faces']=faces;result['receiver']=face
    stage('native no-paint baseline');evidence('baseline')
    stage('flat detail preserves live normal')
    stamp('materials/decalpaint/hard_round.azmaterial',normal_opacity=1.)
    wait(render_ready);evidence('flat')
    result['flat_normal_check']=compare('baseline','flat',neutral=True,painted=True)
    stage('normal detail and erase flatten into source texels')
    material='assets/materials/decalpaint/surfacetexture_0002/surfacetexture_0002.azmaterial'
    for angle in (0.,45.,90.):stamp(material,diameter=.35,normal_opacity=.8,rotation=angle)
    before_erase=get_document();before_coverage=coverage(before_erase)
    # Reuse the last stamp's exact alpha footprint, guaranteeing overlap with
    # painted normal texels rather than assuming the image center is opaque.
    document=stamp(material,diameter=.35,opacity=.65,rotation=90.,erase=True)
    after_coverage=coverage(document)
    assert after_coverage[0]<before_coverage[0] and after_coverage[1]<before_coverage[1],(before_coverage,after_coverage)
    result['erase_reduces_color_and_normal_coverage']=True
    stage('undo and redo restore saved texture snapshots')
    general.undo();general.idle_wait_frames(8)
    assert get_document()['ClassData']['SurfacePaint']==before_erase['ClassData']['SurfacePaint'],'Undo did not restore pre-erase texels'
    general.redo();general.idle_wait_frames(8)
    assert get_document()['ClassData']['SurfacePaint']==document['ClassData']['SurfacePaint'],'Redo did not restore erased texels'
    result['texture_snapshot_undo_redo_exact']=True
    assert retained_dabs(document)==0
    assert all('Material' not in c for c in document['ClassData']['SurfacePaint'])
    result['retained_dabs']=0;result['charts']=len(document['ClassData']['SurfacePaint'])
    painted_faces={c['Face'] for c in document['ClassData']['SurfacePaint']}
    result['surface_faces']=[f for f in faces if f['face'] in painted_faces]
    (OUTPUT/'paint-document.json').write_text(json.dumps(document,indent=2)+'\n')
    stage('serialize clear and restore authoritative source')
    put(empty)
    assert not get_document()['ClassData'].get('SurfacePaint')
    put(json.loads((OUTPUT/'paint-document.json').read_text()))
    restored=get_document()
    assert restored['ClassData']['SurfacePaint']==document['ClassData']['SurfacePaint']
    result['texels_survive_json_roundtrip']=True
    wait(render_ready)
    published_faces=json.loads(silpom.SilPomMeshRequestBus(bus.Event,'GetPaintFacesJson',tower))
    assert {f['face']:f['hit_face'] for f in published_faces}=={f['face']:f['hit_face'] for f in faces},'Receiving generation changed since baseline'
    faces=published_faces;result['faces']=faces
    result['surface_faces']=[f for f in faces if f['face'] in painted_faces]
    result['receiver']=next(f for f in faces if f['face']==face['face'])
    evidence('painted')
    result['painted_output']=compare('baseline','painted',painted=True)
    assert result['painted_output']['normal_changes']>10,'Normal detail made no visible contribution'
    usage=memoryview(dds('painted','usage',3)).cast('I')
    result['authoritative_usage_samples']=sum(usage[(row*5120+column*4+sample)*4]==3
        for row in range(720) for column in range(1280) for sample in (0,1))
    assert result['authoritative_usage_samples']>100,'No authoritative texture use in real MSAA samples'
    stage('clear returns original material')
    put(empty)
    def clear_ready():
        status=json.loads(silpom.SilPomMeshRequestBus(bus.Event,'GetSurfacePaintStatus',tower))
        return status.get('state')=='unbound' or (status.get('ready') and status.get('charts')==0)
    wait(clear_ready)
    evidence('cleared');result['clear_check']=compare('baseline','cleared',identical=True)
    stage('restore has no source stamp dependency')
    put(document);wait(render_ready);evidence('restored')
    result['restore_check']=compare('painted','restored',identical=True)
    result['saved_level_unchanged']=LEVEL.read_bytes()==original
    result['heightmaps_unchanged']=all(hashlib.sha256(p.read_bytes()).hexdigest()==h for p,h in heights.items())
    assert result['saved_level_unchanged'] and result['heightmaps_unchanged']
    log=(PROJECT/'user/log/Editor.log').read_text(errors='replace')
    result['no_skipped_resource_bindings']='Failed to bind shader resources for draw item' not in log
    assert result['no_skipped_resource_bindings']
    result['passed']=True;stage('complete')
except Exception:
    result['error']=traceback.format_exc();stage('failed: '+result['stage'])
finally:
    restore_errors=[]
    for other,saved in hidden_documents:
        error=silpom.DecalPainterBus(bus.Broadcast,'SetPaintDocument',other,json.dumps(saved))
        if error:restore_errors.append(error)
    try:
        if mesh_component is not None and mesh_originals:
            for label in (SHADOW,MATERIAL_A,MATERIAL_B,PROXY,PROXY_DEPTH,LIGHTING,DIRECTIONAL,PROXY_DIRECTIONAL,DIFFUSE):
                set_mesh_value(label,mesh_originals[label])
            assert {label:mesh_value(label) for label in mesh_originals}==mesh_originals,'Mesh settings were not restored exactly'
            wait(lambda:silpom.SilPomMeshRequestBus(bus.Event,'IsReady',tower))
        result['mesh_settings_restored']=True
    except Exception:
        result['mesh_settings_restored']=False;restore_errors.append(traceback.format_exc())
    result['other_paint_documents_restored']=not restore_errors
    if restore_errors:
        result['passed']=False;result['restore_errors']=restore_errors
    (OUTPUT/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    general.run_console('r_silpomMsaaInspect 0')
    print(json.dumps(result,indent=2))
    QtCore.QTimer.singleShot(1000,general.exit_no_prompt)

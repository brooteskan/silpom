"""Native receiver-wide paint budgets, atomic edits and cold publication.

Run in a dedicated rebuilt Editor --runpython session. Existing documents are
temporarily hidden and restored; this script never saves the level or prefab.
"""
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

import azlmbr.bus as bus
import azlmbr.editor as editor
import azlmbr.entity as entity
import azlmbr.legacy.general as general
import azlmbr.math as azmath
import azlmbr.paths as paths
import azlmbr.silpom as silpom
from PySide6 import QtCore

PROJECT=Path(paths.projectroot)
LEVEL=PROJECT/'Levels/VueLiveLink/VueLiveLink.prefab'
OUTPUT=Path(os.environ.get('TG_SILPOM_ESTIMATOR_OUTPUT',str(PROJECT/'user/SurfaceTextureBudgetSmoke')))
OUTPUT.mkdir(parents=True,exist_ok=True)
RESOLUTION=256
WORD_COUNT=RESOLUTION*RESOLUTION*8
CHART_BYTES=sum((RESOLUTION>>level)**2*32 for level in range(9))
result={'passed':False,'stage':'startup','entered_gameplay':False,'benchmark_performed':False,
    'chart_resolution':RESOLUTION,'raw_mip_bytes_per_chart':CHART_BYTES}
hidden_documents=[]
test_documents=[]
payload_cache={}


def stage(name):
    result['stage']=name
    (OUTPUT/'results.json').write_text(json.dumps(result,indent=2)+'\n')


def wait(predicate,seconds=240):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        if predicate():return
        general.idle_wait_frames(1)
    raise TimeoutError(result['stage'])


def raw_document(canvas):
    raw=silpom.DecalPaintRequestBus(bus.Event,'GetDocumentJson',canvas)
    assert raw,'Paint document is unavailable'
    return raw


def document(canvas):return json.loads(raw_document(canvas))
def charts(doc):return doc['ClassData'].get('SurfacePaint',[])


def put(canvas,doc):
    error=silpom.DecalPainterBus(bus.Broadcast,'SetPaintDocument',canvas,json.dumps(doc))
    assert not error,error
    general.idle_wait_frames(8)


def status():
    return json.loads(silpom.SilPomMeshRequestBus(bus.Event,'GetSurfacePaintStatus',tower))


def ready(count):
    current=status();result['last_surface_status']=current
    if not count:
        # Hidden user documents can keep an empty runtime alive. Neither form
        # contains published paint images from the test canvases.
        return current.get('state')=='unbound' or (current.get('ready',False)
            and current.get('charts')==0 and current.get('bytes')==0)
    return current.get('ready',False) and current.get('mode')==3 and current.get('charts')==count and current.get('bytes')==count*CHART_BYTES


def published(name,count):
    wait(lambda:ready(count))
    if not count:general.idle_wait_frames(16) # Drain the renderer's retired paint snapshots too.
    result.setdefault('published_layouts',{})[name]=status()
    assert count*CHART_BYTES<=256*1024*1024


def sparse_payload(x=None,y=None):
    """Independent source encoder, with one opaque texel or an empty image.

    FNV-1a for a zero run is h*prime**bytes modulo 2**32. This avoids iterating
    over two megabytes of identical zero bytes for every sparse source chart.
    """
    key=(x,y)
    if key in payload_cache:return payload_cache[key]
    prime=16777619;mask=0xffffffff;hashed=2166136261;body=bytearray()
    def zeros(words):
        nonlocal hashed
        if words:
            body.extend(struct.pack('<II',0x80000000|words,0))
            hashed=(hashed*pow(prime,words*4,1<<32))&mask
    if x is None:
        zeros(WORD_COUNT)
    else:
        first=(y*RESOLUTION+x)*8
        zeros(first)
        value=struct.pack('<4f',.4,.2,.1,1.)
        body.extend(struct.pack('<I',4));body.extend(value)
        for byte in value:hashed=((hashed^byte)*prime)&mask
        zeros(WORD_COUNT-first-4)
    header=struct.pack('<5I',0x31545053,RESOLUTION,RESOLUTION,WORD_COUNT,hashed)
    encoded=base64.b64encode(header+body).decode('ascii')
    payload_cache[key]=encoded
    return encoded


def candidate(face):
    if face['profile']!=1:return None
    uvs=[c['uv'] for c in face['corners']];positions=[c['position'] for c in face['corners']]
    low=[min(q[i] for q in uvs) for i in range(2)];high=[max(q[i] for q in uvs) for i in range(2)]
    span=[high[i]-low[i] for i in range(2)]
    if min(span)<=1e-8:return None
    center=[sum(q[i] for q in uvs)/3 for i in range(2)]
    atom=[max(0,min(RESOLUTION-1,int((center[i]-low[i])/span[i]*RESOLUTION))) for i in range(2)]
    uv=[low[i]+(atom[i]+.5)/RESOLUTION*span[i] for i in range(2)]
    a=[uvs[1][i]-uvs[0][i] for i in range(2)];b=[uvs[2][i]-uvs[0][i] for i in range(2)]
    c=[uv[i]-uvs[0][i] for i in range(2)];det=a[0]*b[1]-a[1]*b[0]
    if abs(det)<=1e-10:return None
    w1=(c[0]*b[1]-c[1]*b[0])/det;w2=(a[0]*c[1]-a[1]*c[0])/det;weights=(1-w1-w2,w1,w2)
    if min(weights)<=.05:return None
    world=[sum(weights[k]*positions[k][i] for k in range(3)) for i in range(3)]
    distances=[]
    for k in range(3):
        edge=[positions[(k+1)%3][i]-positions[k][i] for i in range(3)]
        delta=[world[i]-positions[k][i] for i in range(3)]
        cross=(edge[1]*delta[2]-edge[2]*delta[1],edge[2]*delta[0]-edge[0]*delta[2],edge[0]*delta[1]-edge[1]*delta[0])
        length=math.sqrt(sum(v*v for v in edge))
        if not length:return None
        distances.append(math.sqrt(sum(v*v for v in cross))/length)
    margin=min(distances)
    if margin<=.2:return None
    return {'face':face['face'],'rectangle':low+high,'atom':atom,'uv':uv,'world':world,'margin':margin}


def choose_faces(faces):
    available=sorted((c for f in faces if (c:=candidate(f)) is not None),key=lambda c:-c['margin'])
    assert len(available)>=98,len(available)
    target=available[0]
    erased=next(c for c in available[1:] if math.dist(c['world'],target['world'])>.8)
    selected=[]
    for c in available:
        if all(math.dist(c['world'],q['world'])>.8 for q in [target,erased]+selected):selected.append(c)
        if len(selected)==95:break
    assert len(selected)==95,('Insufficient separated profile-1 faces',len(selected))
    # The designated erase atom is the first chart in canvas B.
    return selected[:48]+[erased]+selected[48:],target,erased


def stamp(canvas,item,erase=False,diameter=.1):
    return silpom.DecalPainterBus(bus.Broadcast,'ApplySurfaceTextureStamp',canvas,tower,item['face'],
        azmath.Vector2(float(item['uv'][0]),float(item['uv'][1])),azmath.Vector4(diameter,1.,0.,0.),
        azmath.Color_CreateFromVector3AndFloat(azmath.Vector3(.4,.2,.1),1.),
        'materials/decalpaint/hard_round.azmaterial',erase,0)


def chart(item,template):
    value=copy.deepcopy(template)
    value.update({'Face':item['face'],'Rectangle':item['rectangle'],'Resolution':RESOLUTION,
        'Front':True,'Texels':sparse_payload(*item['atom'])})
    return value


def reject_import(canvas,doc,name):
    before=[raw_document(c) for c in (canvas_a,canvas_b)]
    error=silpom.DecalPainterBus(bus.Broadcast,'SetPaintDocument',canvas,json.dumps(doc))
    assert error and '256 MiB' in error and 'canvases' in error,(name,error)
    assert [raw_document(c) for c in (canvas_a,canvas_b)]==before,(name,'Rejected edit changed source documents')
    result[name]={'rejected':True,'documents_bit_exact':True,'error':error}


try:
    original=LEVEL.read_bytes();result['level_sha256']=hashlib.sha256(original).hexdigest()
    heights={p:hashlib.sha256(p.read_bytes()).hexdigest() for p in (PROJECT/'Assets/SilPOMPlanarAB').rglob('height.png')}
    assert heights
    general.idle_enable(True)
    for command in ('ed_keepEditorActive 1','ed_backgroundUpdatePeriod -1','r_displayInfo 0','r_silpomMsaaInspect 0'):
        general.run_console(command)
    stage('load unchanged source level')
    assert general.open_level(str(LEVEL));general.idle_wait_frames(120)
    query=entity.SearchFilter();query.names=['Irish Fieldstone Tower (Open Shaft)']
    found=entity.SearchBus(bus.Broadcast,'SearchEntities',query);assert found
    tower=found[0]
    wait(lambda:silpom.SilPomMeshRequestBus(bus.Event,'IsReady',tower));general.idle_wait_frames(60)
    faces=json.loads(silpom.SilPomMeshRequestBus(bus.Event,'GetPaintFacesJson',tower))
    selected,target,erased=choose_faces(faces)
    result['selected_faces']=[{'face':c['face'],'uv':c['uv'],'atom':c['atom']} for c in selected]
    result['extra_face']={'face':target['face'],'uv':target['uv']}
    stage('temporarily hide other authoritative paint documents')
    types=editor.EditorComponentAPIBus(bus.Broadcast,'FindComponentTypeIdsByEntityType',['Surface Decal Paint'],entity.EntityType().Game)
    assert types
    query=entity.SearchFilter();query.components={types[0]:{}}
    for other in entity.SearchBus(bus.Broadcast,'SearchEntities',query):
        raw=silpom.DecalPaintRequestBus(bus.Event,'GetDocumentJson',other)
        if not raw:continue
        saved=json.loads(raw)
        if not saved['ClassData'].get('TextureAuthoritative'):continue
        hidden_documents.append((other,saved))
        muted=copy.deepcopy(saved)
        for layer in muted['ClassData'].get('Layers',[]):layer['Visible']=False
        put(other,muted)
    result['other_authoritative_documents_hidden']=len(hidden_documents)
    canvas_a=silpom.DecalPainterBus(bus.Broadcast,'CreatePaintEntity');assert canvas_a.IsValid()
    canvas_b=silpom.DecalPainterBus(bus.Broadcast,'CreatePaintEntity');assert canvas_b.IsValid() and canvas_b!=canvas_a
    editor.EditorEntityAPIBus(bus.Event,'SetName',canvas_a,'Surface Texture Budget A')
    editor.EditorEntityAPIBus(bus.Event,'SetName',canvas_b,'Surface Texture Budget B')
    empty_a=document(canvas_a);empty_b=document(canvas_b)
    test_documents=[(canvas_a,empty_a),(canvas_b,empty_b)]
    assert empty_a['ClassData'].get('TextureAuthoritative') and empty_b['ClassData'].get('TextureAuthoritative')
    stage('obtain native receiver and layer serialization template')
    error=stamp(canvas_b,erased,diameter=.45);assert not error,error
    template=next(c for c in charts(document(canvas_b)) if c['Face']==erased['face'])
    put(canvas_b,empty_b);published('initial_empty',0)
    initial_a=copy.deepcopy(empty_a);initial_b=copy.deepcopy(empty_b)
    initial_a['ClassData']['SurfacePaint']=[chart(c,template) for c in selected[:48]]
    initial_b['ClassData']['SurfacePaint']=[chart(c,template) for c in selected[48:]]
    stage('publish two canvases with 48 plus 48 charts')
    put(canvas_a,initial_a);published('canvas_a_48',48)
    put(canvas_b,initial_b);published('combined_96',96)
    assert 96*CHART_BYTES==268434432
    stage('reject the 97th chart across canvases')
    over=copy.deepcopy(document(canvas_b));over['ClassData']['SurfacePaint'].append(chart(target,template))
    reject_import(canvas_b,over,'cross_canvas_import_rejected')
    before=[raw_document(c) for c in (canvas_a,canvas_b)]
    error=stamp(canvas_b,target)
    assert error and '256 MiB' in error and 'canvases' in error,error
    assert [raw_document(c) for c in (canvas_a,canvas_b)]==before,'Rejected native brush changed paint documents'
    result['cross_canvas_brush_rejected']={'rejected':True,'documents_bit_exact':True,'error':error}
    published('rejected_edits_keep_96',96)
    stage('full erase reclaims a chart and permits new paint')
    error=stamp(canvas_b,erased,erase=True);assert not error,error
    assert len(charts(document(canvas_b)))==47
    assert erased['face'] not in {c['Face'] for c in charts(document(canvas_b))}
    published('erase_reclaimed_95',95)
    error=stamp(canvas_b,target);assert not error,error
    assert len(charts(document(canvas_b)))==48
    published('new_brush_after_erase_96',96)
    result['erase_reclaims_chart_slot_and_bytes']=True
    stage('empty imported charts are pruned before budget validation')
    replace=copy.deepcopy(document(canvas_b));zeroed=replace['ClassData']['SurfacePaint'][1]['Face']
    replace['ClassData']['SurfacePaint'][1]['Texels']=sparse_payload()
    replace['ClassData']['SurfacePaint'].append(chart(erased,template))
    put(canvas_b,replace)
    assert len(charts(document(canvas_b)))==48 and zeroed not in {c['Face'] for c in charts(document(canvas_b))}
    published('empty_import_reclaimed_96',96)
    result['empty_import_pruned_before_capacity_check']=True
    saved_b=document(canvas_b);all_empty=copy.deepcopy(saved_b)
    for item in charts(all_empty):item['Texels']=sparse_payload()
    put(canvas_b,all_empty);assert not charts(document(canvas_b));published('all_empty_import_48',48)
    put(canvas_b,saved_b);published('restore_empty_import_96',96)
    result['all_empty_import_reclaims_all_chart_bytes']=True
    stage('unhide cannot overfill capacity consumed by another canvas')
    shown_a=document(canvas_a);hidden_a=copy.deepcopy(shown_a)
    for layer in hidden_a['ClassData']['Layers']:layer['Visible']=False
    put(canvas_a,hidden_a);published('hidden_canvas_a_48',48)
    item=next(c for c in selected if c['face']==zeroed)
    extra_b=copy.deepcopy(document(canvas_b));extra_b['ClassData']['SurfacePaint'].append(chart(item,template))
    put(canvas_b,extra_b);published('hidden_a_plus_b_49',49)
    reject_import(canvas_a,shown_a,'cross_canvas_unhide_rejected')
    put(canvas_b,saved_b);put(canvas_a,shown_a);published('visibility_capacity_restored_96',96)
    stage('cold publication from serialized source without a previous snapshot')
    saved_a=document(canvas_a);saved_b=document(canvas_b)
    for label,doc in (('canvas-a',saved_a),('canvas-b',saved_b)):
        (OUTPUT/(label+'.json')).write_text(json.dumps(doc,indent=2)+'\n')
    put(canvas_a,empty_a);published('cold_clear_a_48',48)
    put(canvas_b,empty_b);published('cold_unbound',0)
    put(canvas_a,json.loads((OUTPUT/'canvas-a.json').read_text()));published('cold_restore_a_48',48)
    put(canvas_b,json.loads((OUTPUT/'canvas-b.json').read_text()));published('cold_restore_combined_96',96)
    assert charts(document(canvas_a))==charts(saved_a) and charts(document(canvas_b))==charts(saved_b)
    result['cold_publish_from_saved_texels']=True
    result['retained_dabs']=sum(silpom.DecalPaintRequestBus(bus.Event,'GetDabCount',c)+silpom.DecalPaintRequestBus(bus.Event,'GetCompositeDabCount',c) for c in (canvas_a,canvas_b))
    assert result['retained_dabs']==0
    result['saved_level_unchanged']=LEVEL.read_bytes()==original
    result['heightmaps_unchanged']=all(hashlib.sha256(p.read_bytes()).hexdigest()==value for p,value in heights.items())
    assert result['saved_level_unchanged'] and result['heightmaps_unchanged']
    result['no_skipped_resource_bindings']='Failed to bind shader resources for draw item' not in (PROJECT/'user/log/Editor.log').read_text(errors='replace')
    assert result['no_skipped_resource_bindings']
    # Publish success only after finally has released test capacity and restored
    # the pre-existing paint documents.
    result['passed']=True;result['stage']='complete'
except Exception:
    result['error']=traceback.format_exc();stage('failed: '+result['stage'])
finally:
    restore_errors=[]
    # Release the test capacity before re-enabling any saved user paint.
    for canvas,empty in test_documents:
        error=silpom.DecalPainterBus(bus.Broadcast,'SetPaintDocument',canvas,json.dumps(empty))
        if error:restore_errors.append(error)
    general.idle_wait_frames(8)
    for other,saved in hidden_documents:
        error=silpom.DecalPainterBus(bus.Broadcast,'SetPaintDocument',other,json.dumps(saved))
        if error:restore_errors.append(error)
        elif document(other)!=saved:restore_errors.append('Existing authoritative paint document did not restore exactly')
    result['other_authoritative_documents_restored']=not restore_errors
    if restore_errors:result['passed']=False;result['restore_errors']=restore_errors
    if 'original' in globals():
        result['saved_level_unchanged']=LEVEL.read_bytes()==original
        if not result['saved_level_unchanged']:result['passed']=False
    (OUTPUT/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
    QtCore.QTimer.singleShot(1000,general.exit_no_prompt)

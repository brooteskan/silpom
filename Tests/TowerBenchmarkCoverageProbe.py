"""Run via qrenderdoc --python with TG_SILPOM_COVERAGE_CONFIG JSON path.

Read-only pipeline, post-VS and optional before/after depth extraction.
Requests no counters and needs no Developer Mode or persistent setting changes.
"""
from pathlib import Path
import enum
import hashlib
import json
import os
import shutil
import sys
import traceback
import renderdoc as rd

config = json.loads(Path(os.environ['TG_SILPOM_COVERAGE_CONFIG']).read_text(encoding='utf-8-sig'))
output = Path(config['output'])
result = {'passed': False, 'event': int(config.get('event',464)), 'scope': 'Read-only replay and diagnostic mesh readbacks; no baseline timing or shader edits'}
cap = controller = setting = None
old = None

def encode(value, depth=0):
    if isinstance(value, enum.IntEnum): return str(value)
    if isinstance(value, (str, int, float, bool)) or value is None: return value
    if isinstance(value, rd.ResourceId): return str(value)
    if depth > 6: return str(type(value))
    if hasattr(value, '__len__') and hasattr(value, '__getitem__'):
        return [encode(v,depth+1) for v in value[:128]]
    data = {}
    for name in dir(value):
        if name.startswith('_') or name in ('this','thisown'): continue
        item = getattr(value,name)
        if not callable(item): data[name] = encode(item,depth+1)
    return data or str(value)

def save(stage):
    result['stage'] = stage
    (output / 'coverage-probe.json').write_text(json.dumps(result,indent=2)+'\n')

def healthy():
    status = controller.GetFatalErrorStatus()
    result['fatal_status'] = str(status)
    assert status == rd.ResultCode.Succeeded, str(status)

try:
    output.mkdir(parents=True,exist_ok=True)
    result['renderdoc_log'] = rd.GetLogFile()
    metadata = json.loads(Path(config['capture_metadata']).read_text(encoding='utf-8-sig'))
    assert metadata['passed'] and metadata['restored']
    assert metadata['renderdoc']['file']==Path(config['capture']).name
    samples=int(config.get('samples',metadata.get('samples',1)))
    assert samples in (1,2)
    assert metadata['initial_aa_state']==[samples,0,0]
    result['sample_count']=samples
    assert metadata['controls']['Height-derived normals (off = cheaper)'] is True
    assert metadata['controls']['Gather validation (may be cheaper)'] is True
    assert metadata['controls']['R16 height textures (less GPU memory)'] is True
    result['controls'] = metadata['controls']
    result['capture_sha256']=hashlib.sha256(Path(config['capture']).read_bytes()).hexdigest()
    result['scene_sha256']=metadata['scene_sha256']
    result['effective_configuration_sha256']=metadata['effective_tower_configuration_sha256']
    setting = rd.SetConfigSetting('D3D12.BindlessFeedback')
    old = setting.AsBool(); setting.data.basic.b = False
    cap = rd.OpenCaptureFile()
    assert cap.OpenFile(config['capture'],'',None) == rd.ResultCode.Succeeded
    opts=rd.ReplayOptions();opts.forceGPUVendor=rd.GPUVendor.AMD;opts.forceGPUDeviceID=0x1638
    status,controller=cap.OpenCapture(opts,None)
    assert status == rd.ResultCode.Succeeded,str(status)
    healthy();save('replay-opened')
    if config.get('auto_indices'):
        actions=[]
        def collect(items):
            for item in items:
                actions.append(item);collect(item.children)
        collect(controller.GetRootActions())
        candidates=[]
        for item in actions:
            if item.numIndices!=int(config['auto_indices']) or 'Drawcall' not in str(item.flags):continue
            controller.SetFrameEvent(item.eventId,True);healthy()
            reflection=controller.GetPipelineState().GetShaderReflection(rd.ShaderStage.Pixel)
            if reflection and any(str(p.systemValue)=='ShaderBuiltin.DepthOutputLessEqual' for p in reflection.outputSignature):
                candidates.append(item.eventId)
        selected=config.get('event')
        assert (int(selected) in candidates) if selected is not None else len(candidates)==1, \
            ('Specify an event from conservative-depth proxy candidates',candidates)
        result['event']=int(selected) if selected is not None else candidates[0]
        result['conservative_depth_candidates']=candidates
        result['auto_selected_indices']=int(config['auto_indices'])
        result['preceding_action']=max(item.eventId for item in actions if item.eventId<result['event']
            and any(flag in str(item.flags) for flag in ('Drawcall','Clear')))
    controller.SetFrameEvent(result['event'],True);healthy()
    def find(items):
        for item in items:
            if item.eventId==result['event']:return item
            nested=find(item.children)
            if nested is not None:return nested
        return None
    action=find(controller.GetRootActions())
    result['action']={name:encode(getattr(action,name,None)) for name in
        ('eventId','flags','numIndices','numInstances','indexOffset','vertexOffset','baseVertex','instanceOffset','drawIndex')}
    state=controller.GetD3D12PipelineState()
    result['rasterizer']=encode(state.rasterizer)
    result['output_merger']=encode(state.outputMerger)
    result['input_assembly']=encode(state.inputAssembly)
    pipe=controller.GetPipelineState()
    result['vertex_shader']=str(pipe.GetShader(rd.ShaderStage.Vertex))
    result['pixel_shader']=str(pipe.GetShader(rd.ShaderStage.Pixel))
    result['vs_signature']=encode(pipe.GetShaderReflection(rd.ShaderStage.Vertex).outputSignature)
    result['ps_signature']=encode(pipe.GetShaderReflection(rd.ShaderStage.Pixel).outputSignature)
    save('pipeline-read')
    mesh=controller.GetPostVSData(0,0,rd.MeshDataStage.VSOut);healthy()
    result['postvs']=encode(mesh);save('postvs-created')
    vb=bytes(controller.GetBufferData(mesh.vertexResourceId,mesh.vertexByteOffset,mesh.vertexByteSize));healthy()
    (output/'postvs-vertices.bin').write_bytes(vb)
    result['postvs_vertex_bytes']=len(vb)
    if mesh.indexResourceId != rd.ResourceId.Null():
        ib=bytes(controller.GetBufferData(mesh.indexResourceId,mesh.indexByteOffset,mesh.indexByteSize));healthy()
        (output/'postvs-indices.bin').write_bytes(ib)
        result['postvs_index_bytes']=len(ib)
    if config.get('depth_readbacks',False):
        depth_id=state.outputMerger.depthTarget.resource
        result['depth_texture']=encode(next(t for t in controller.GetTextures() if t.resourceId==depth_id))
        assert result['depth_texture']['msSamp']==samples
        before_event=int(config.get('before_event',result.get('preceding_action',460)))
        result['before_event']=before_event
        for label,event in (('before',before_event),('after',result['event'])):
            save('depth-'+label)
            controller.SetFrameEvent(event,True);healthy()
            result['depth_'+label+'_samples']=[]
            for sample in range(samples):
                subresource=rd.Subresource();subresource.sample=sample
                data=bytes(controller.GetTextureData(depth_id,subresource));healthy()
                filename='depth-'+label+('' if samples==1 else '-sample-'+str(sample))+'.bin'
                (output/filename).write_bytes(data)
                result['depth_'+label+'_samples'].append({'sample':sample,'file':filename,
                    'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
            result['depth_'+label+'_bytes']=sum(s['bytes'] for s in result['depth_'+label+'_samples'])
    result['passed']=True;save('complete')
except Exception:
    result['error']=traceback.format_exc();save('failed')
finally:
    if controller is not None:
        result['final_fatal_status']=str(controller.GetFatalErrorStatus());controller.Shutdown()
    if cap is not None:cap.Shutdown()
    if setting is not None and old is not None:setting.data.basic.b=old
    log=result.get('renderdoc_log')
    if log and Path(log).is_file():shutil.copyfile(log,str(output/'renderdoc.log'))
    (output/'coverage-probe.json').write_text(json.dumps(result,indent=2)+'\n')
sys.exit(0)

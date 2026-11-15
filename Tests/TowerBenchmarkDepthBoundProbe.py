"""Read captured constants and buffer metadata for offline conservative bounds.

Run through qrenderdoc --python, TG_SILPOM_COVERAGE_CONFIG points to JSON.
No counters, shader replacement, Editor, or Windows setting changes.
"""
from pathlib import Path
import enum
import hashlib
import json
import os
import sys
import traceback
import struct
import renderdoc as rd

config=json.loads(Path(os.environ['TG_SILPOM_COVERAGE_CONFIG']).read_text(encoding='utf-8-sig'))
output=Path(config['output'])
result={'passed':False,'event':int(config['event']),'scope':__doc__}
cap=controller=setting=None
old=None

def encode(value,depth=0):
    if isinstance(value,enum.IntEnum):return str(value)
    if isinstance(value,(str,int,float,bool)) or value is None:return value
    if isinstance(value,rd.ResourceId):return str(value)
    if depth>7:return str(type(value))
    if hasattr(value,'__len__') and hasattr(value,'__getitem__'):
        return [encode(v,depth+1) for v in value[:256]]
    return {name:encode(getattr(value,name),depth+1) for name in dir(value)
        if not name.startswith('_') and name not in ('this','thisown') and not callable(getattr(value,name))}

def healthy():
    status=controller.GetFatalErrorStatus();result['fatal_status']=str(status)
    assert status==rd.ResultCode.Succeeded,str(status)

def save(stage):
    result['stage']=stage
    (output/'depth-bound-probe.json').write_text(json.dumps(result,indent=2)+'\n')

try:
    output.mkdir(parents=True,exist_ok=True)
    save('startup')
    metadata=json.loads(Path(config['capture_metadata']).read_text(encoding='utf-8-sig'))
    assert metadata['passed'] and metadata['restored'] and metadata['initial_aa_state']==[2,0,0]
    assert metadata['controls']['Height-derived normals (off = cheaper)'] is True
    result['controls']=metadata['controls']
    result['capture_sha256']=hashlib.sha256(Path(config['capture']).read_bytes()).hexdigest()
    setting=rd.SetConfigSetting('D3D12.BindlessFeedback');old=setting.AsBool();setting.data.basic.b=False
    cap=rd.OpenCaptureFile();assert cap.OpenFile(config['capture'],'',None)==rd.ResultCode.Succeeded
    opts=rd.ReplayOptions();opts.forceGPUVendor=rd.GPUVendor.AMD;opts.forceGPUDeviceID=0x1638
    status,controller=cap.OpenCapture(opts,None);assert status==rd.ResultCode.Succeeded,str(status)
    save('replay-opened')
    healthy();controller.SetFrameEvent(result['event'],True);healthy()
    pipe=controller.GetPipelineState();state=controller.GetD3D12PipelineState()
    result['api_docs']={name:getattr(controller,name).__doc__ for name in
        ('GetCBufferVariableContents','GetDescriptors','GetDescriptorLocations','GetBufferData') if hasattr(controller,name)}
    result['pipe_docs']={name:getattr(pipe,name).__doc__ for name in dir(pipe)
        if ('Constant' in name or 'Resource' in name or 'Descriptor' in name) and callable(getattr(pipe,name))}
    reflection=pipe.GetShaderReflection(rd.ShaderStage.Vertex)
    result['constant_blocks']=encode(reflection.constantBlocks)
    result['pipeline_fields']=[name for name in dir(state) if not name.startswith('_')]
    result['root_signature']=encode(state.rootSignature)
    result['resources']=[{'id':str(r.resourceId),'name':r.name} for r in controller.GetResources()]
    names={r.resourceId:r.name for r in controller.GetResources()}
    result['buffers']=[{'id':str(b.resourceId),'name':names.get(b.resourceId,''),
        'length':b.length} for b in controller.GetBuffers()]
    result['constant_values']={}
    def variable(v):
        item={'name':v.name,'rows':v.rows,'columns':v.columns,'type':str(v.type)}
        if v.members:item['members']=[variable(m) for m in v.members]
        else:
            count=v.rows*v.columns
            item['values']=list(v.value.f32v[:count] if v.type==rd.VarType.Float else v.value.u32v[:count])
        return item
    for index,block in enumerate(reflection.constantBlocks):
        bound=pipe.GetConstantBlock(rd.ShaderStage.Vertex,index,0)
        result['constant_values'][block.name]={'binding':encode(bound)}
        desc=bound.descriptor
        values=controller.GetCBufferVariableContents(pipe.GetGraphicsPipelineObject(),
            pipe.GetShader(rd.ShaderStage.Vertex),rd.ShaderStage.Vertex,reflection.entryPoint,
            index,desc.resource,desc.byteOffset,desc.byteSize)
        result['constant_values'][block.name]['variables']=[variable(v) for v in values]
        if desc.resource!=rd.ResourceId.Null():
            data=bytes(controller.GetBufferData(desc.resource,desc.byteOffset,block.byteSize));healthy()
            (output/(block.name+'.bin')).write_bytes(data)
    save('constants-read')
    result['descriptor_range_fields']=[n for n in dir(rd.DescriptorRange()) if not n.startswith('_')]
    result['descriptor_tables']=[]
    for i,param in enumerate(state.rootSignature.parameters):
        if param.heap==rd.ResourceId.Null():continue
        count=int(config.get('bindless_count',2048)) if any(r.space==1002 for r in param.tableRanges) else max(r.tableByteOffset+r.count for r in param.tableRanges)
        if count>4096:continue
        span=rd.DescriptorRange();span.offset=param.heapByteOffset;span.descriptorSize=1;span.count=count
        descriptors=controller.GetDescriptors(param.heap,[span]);healthy()
        result['descriptor_tables'].append({'root':i,'start':param.heapByteOffset,
            'descriptors':[encode(d) for d in descriptors]})
    save('descriptors-read')
    # Export only buffer descriptors from the captured bindless table. No shader
    # instrumentation or descriptor feedback is requested.
    result['buffer_exports']=[]
    for table in result['descriptor_tables']:
        for index,desc in enumerate(table['descriptors']):
            if table['root']==10 and config.get('export_bindless_indices') and index not in config['export_bindless_indices']:continue
            if desc['resource']=='ResourceId::0' or desc['type'] not in ('DescriptorType.Buffer','DescriptorType.TypedBuffer'):continue
            resource=next((b.resourceId for b in controller.GetBuffers() if str(b.resourceId)==desc['resource']),None)
            if resource is None:continue
            count=min(desc['byteSize'],4*1024*1024) if desc['byteSize'] else 0
            data=bytes(controller.GetBufferData(resource,desc['byteOffset'],count));healthy()
            filename='root%d-descriptor%d.bin'%(table['root'],index)
            (output/filename).write_bytes(data)
            result['buffer_exports'].append({'root':table['root'],'index':index,'file':filename,
                'resource':desc['resource'],'byte_offset':desc['byteOffset'],'bytes':len(data)})
    if config.get('export_hit_records'):
        target=state.outputMerger.renderTargets[0].resource
        texture=next(t for t in controller.GetTextures() if t.resourceId==target)
        result['hit_texture']=encode(texture)
        assert texture.width==1280 and texture.height==720 and texture.msSamp==2
        result['hit_records']=[]
        for sample in range(2):
            sub=rd.Subresource();sub.sample=sample
            data=bytes(controller.GetTextureData(target,sub));healthy()
            assert len(data)==1280*720*16
            filename='hit-records-sample-%d.bin'%sample
            (output/filename).write_bytes(data)
            result['hit_records'].append({'sample':sample,'file':filename,'bytes':len(data),
                'sha256':hashlib.sha256(data).hexdigest()})
    result['passed']=True;save('complete')
except Exception:
    result['error']=traceback.format_exc();save('failed')
finally:
    if controller is not None:result['final_fatal_status']=str(controller.GetFatalErrorStatus());controller.Shutdown()
    if cap is not None:cap.Shutdown()
    if setting is not None and old is not None:setting.data.basic.b=old
    (output/'depth-bound-probe.json').write_text(json.dumps(result,indent=2)+'\n')
sys.exit(0)

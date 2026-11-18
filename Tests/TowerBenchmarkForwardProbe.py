"""Read final hit ownership for retained forward draws, without GPU counters.

Run with qrenderdoc --python; SILPOM_FORWARD_CONFIG names a JSON containing
profile (the retained profile.json), matches (shader-product-matches.json), and
output. Material offsets below are the captured SM6.2 layout, checked against
the exported disassembly; this probe intentionally targets that tower capture.
"""
from pathlib import Path
import collections
import hashlib
import json
import os
import statistics
import struct
import sys
import traceback

import renderdoc as rd


config = json.loads(Path(os.environ['SILPOM_FORWARD_CONFIG']).read_text(encoding='utf-8-sig'))
output = Path(config['output'])
output.mkdir(parents=True, exist_ok=True)
result = {'passed': False, 'scope': 'Retained stable-power draw timings; read-only final hit ownership, no counters'}
cap = controller = setting = None
old = None


def save(stage):
    result['stage'] = stage
    (output / 'forward-draws.json').write_text(json.dumps(result, indent=2) + '\n')


def healthy():
    assert controller.GetFatalErrorStatus() == rd.ResultCode.Succeeded


def read_buffer(desc, offset, size):
    assert offset + size <= desc.byteSize
    data = bytes(controller.GetBufferData(desc.resource, desc.byteOffset + offset, size))
    healthy()
    assert len(data) == size
    return data


try:
    profile = json.loads(Path(config['profile']).read_text())
    matches = json.loads(Path(config['matches']).read_text())['matches']
    assert profile['passed'] and profile['sample_count'] == 2
    capture = Path(profile['capture'])
    result['capture'] = str(capture)
    result['capture_sha256'] = hashlib.sha256(capture.read_bytes()).hexdigest()
    assert result['capture_sha256'] == profile['capture_sha256']
    result['profile_sha256'] = hashlib.sha256(Path(config['profile']).read_bytes()).hexdigest()
    draws = []
    for draw in profile['draws']:
        shader = draw.get('shaders', {}).get('ShaderStage.Pixel')
        products = sorted({p['product'] for p in matches.get(shader, [])})
        if any('silpomhitforward' in p for p in products):
            assert len(products) == 1, products
            draws.append(dict(draw, products=products))
    assert len(draws) == 9
    setting = rd.SetConfigSetting('D3D12.BindlessFeedback')
    old = setting.AsBool()
    setting.data.basic.b = False
    cap = rd.OpenCaptureFile()
    assert cap.OpenFile(str(capture), '', None) == rd.ResultCode.Succeeded
    options = rd.ReplayOptions()
    options.forceGPUVendor = rd.GPUVendor.AMD
    options.forceGPUDeviceID = 0x1638
    status, controller = cap.OpenCapture(options, None)
    assert status == rd.ResultCode.Succeeded, str(status)
    result['draws'] = []
    owners = None
    for draw in sorted(draws, key=lambda d: d['event']):
        save('event-' + str(draw['event']))
        controller.SetFrameEvent(draw['event'], True)
        healthy()
        pipe = controller.GetPipelineState()
        stage = rd.ShaderStage.Pixel
        reflection = pipe.GetShaderReflection(stage)
        assert str(pipe.GetShader(stage)) == draw['shaders']['ShaderStage.Pixel']
        index = next(i for i, b in enumerate(reflection.constantBlocks) if b.name == 'ObjectSrg_SRGConstantBuffer')
        desc = pipe.GetConstantBlock(stage, index, 0).descriptor
        object_id, type_id, material_id = struct.unpack('<III', read_buffer(desc, 0, 12))
        state = controller.GetD3D12PipelineState()

        def resource(name):
            shader_resource = next(r for r in reflection.readOnlyResources if r.name == name)
            found = []
            for parameter in state.rootSignature.parameters:
                for binding in parameter.tableRanges:
                    if (binding.category == rd.DescriptorCategory.ReadOnlyResource
                            and binding.space == shader_resource.fixedBindSetOrSpace
                            and binding.baseRegister <= shader_resource.fixedBindNumber < binding.baseRegister + binding.count):
                        span = rd.DescriptorRange()
                        span.offset = parameter.heapByteOffset + binding.tableByteOffset + shader_resource.fixedBindNumber - binding.baseRegister
                        span.descriptorSize = span.count = 1
                        found.extend(controller.GetDescriptors(parameter.heap, [span]))
            assert len(found) == 1, (name, len(found))
            return found[0]

        types = resource('SceneMaterialSrg_m_materialTypeBufferIndices')
        material_buffer_index, = struct.unpack('<I', read_buffer(types, type_id * 4, 4))
        root = next(p for p in state.rootSignature.parameters
                    if any(r.space == 1002 for r in p.tableRanges))

        def bindless(index):
            span = rd.DescriptorRange()
            span.offset = root.heapByteOffset + index
            span.descriptorSize = 1
            span.count = 1
            return controller.GetDescriptors(root.heap, [span])[0]

        material = read_buffer(bindless(material_buffer_index), material_id * 576, 576)
        assert struct.unpack_from('<II', material) == (type_id, material_id)
        max_cells, = struct.unpack_from('<I', material, 372)
        mesh_index, face_base = struct.unpack_from('<II', material, 456)
        mesh = bindless(mesh_index)
        assert mesh.byteSize % 144 == 0
        face_count = mesh.byteSize // 144
        # Each face rasterizes a conservative quad (four vertices, six indices).
        assert face_count * 6 == draw['indices']
        if owners is None:
            hit = resource('PassSrg_m_silpomHitBuffer')
            texture = next(t for t in controller.GetTextures() if t.resourceId == hit.resource)
            assert (texture.width, texture.height, texture.msSamp) == (1280, 720, 2)
            owners = collections.Counter()
            result['hit_records'] = []
            for sample in range(2):
                sub = rd.Subresource()
                sub.sample = sample
                data = bytes(controller.GetTextureData(hit.resource, sub))
                healthy()
                assert len(data) == 1280 * 720 * 16
                filename = 'final-hit-sample-%d.bin' % sample
                (output / filename).write_bytes(data)
                result['hit_records'].append({'sample': sample, 'file': filename,
                    'sha256': hashlib.sha256(data).hexdigest()})
                for _, _, _, packed in struct.iter_unpack('<4I', data):
                    if packed & 3 == 1:
                        owners[packed >> 2] += 1
            result['valid_final_hit_samples'] = sum(owners.values())
        owned = sum(n for face, n in owners.items() if face_base <= face < face_base + face_count)
        rounds = [r['events'][str(draw['event'])] for r in profile['rounds']]
        result['draws'].append(dict(draw, object_id=object_id, material_type=type_id,
            material_id=material_id, mesh_buffer_index=mesh_index, face_base=face_base,
            face_count=face_count, max_cells=max_cells, owned_final_hit_samples=owned,
            owned_viewport_fraction=owned / (1280 * 720 * 2),
            samples_passed=[r['Samples Passed'] for r in rounds]))
    ranges = sorted((d['face_base'], d['face_base'] + d['face_count']) for d in result['draws'])
    assert all(a[1] <= b[0] for a, b in zip(ranges, ranges[1:]))
    assert sum(d['owned_final_hit_samples'] for d in result['draws']) == result['valid_final_hit_samples']
    angular = [d for d in result['draws'] if 'angular' in d['products'][0]]
    sums = [sum(d['duration_ms'][i] for d in angular) for i in range(len(profile['rounds']))]
    result['angular_duration_sums_ms'] = sums
    result['angular_median_sum_ms'] = statistics.median(sums)
    result['passed'] = True
    save('complete')
except Exception:
    result['error'] = traceback.format_exc()
    save('failed')
finally:
    if controller is not None:
        result['final_fatal_status'] = str(controller.GetFatalErrorStatus())
        controller.Shutdown()
    if cap is not None:
        cap.Shutdown()
    if setting is not None and old is not None:
        setting.data.basic.b = old
    save(result.get('stage', 'failed'))
sys.exit(0)

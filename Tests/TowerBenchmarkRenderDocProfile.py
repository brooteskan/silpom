"""Run with qrenderdoc --python and SILPOM_RENDERDOC_CONFIG pointing to JSON.

Input keys: capture, capture_metadata, output. Optional repeats (minimum 3),
shader_export (default true), gpu_device_id (default 0x1638), samples (1 or 2).
A batch configuration has output and profiles (a list of individual inputs).
Never saves a scene, edits a shader, changes Windows settings or requests
RenderDoc's instrumented bindless-descriptor feedback.
"""
from pathlib import Path
import collections
import hashlib
import json
import os
import shutil
import statistics
import sys
import traceback
import winreg

import renderdoc as rd


def developer_mode_enabled():
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                r'SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock') as key:
            return winreg.QueryValueEx(key, 'AllowDevelopmentWithoutDevLicense')[0] == 1
    except OSError:
        return False


def counter_value(result, description):
    # CompType.Float includes both float32 and float64. GPU Duration is float64.
    if description.resultType == rd.CompType.Float:
        return result.value.d if description.resultByteWidth == 8 else result.value.f
    return result.value.u64 if description.resultByteWidth == 8 else result.value.u32


def profile(config):
    output = Path(config['output'])
    output.mkdir(parents=True, exist_ok=True)
    result = {'passed': False, 'scope': 'Separate captured-frame replay under RenderDoc stable power state; excluded from baseline statistics',
              'capture': str(config['capture']), 'rounds': [], 'draws': [], 'shaders': {}}
    cap = controller = feedback = None
    original_feedback = None

    def save(stage):
        result['stage'] = stage
        (output / 'profile.json').write_text(json.dumps(result, indent=2) + '\n')

    def healthy():
        status = controller.GetFatalErrorStatus()
        result['fatal_status'] = str(status)
        assert status == rd.ResultCode.Succeeded, str(status)

    try:
        save('preflight')
        repeats = int(config.get('repeats', 3))
        assert repeats >= 3, 'At least three measured replays required'
        metadata = json.loads(Path(config['capture_metadata']).read_text(encoding='utf-8-sig'))
        assert metadata['passed'] and metadata['restored']
        assert metadata['renderdoc']['file'] == Path(config['capture']).name
        samples = int(config.get('samples', metadata.get('samples', 1)))
        assert samples in (1, 2)
        assert metadata['initial_aa_state'] == [samples, 0, 0]
        result['sample_count'] = samples
        result['aa_state'] = metadata['initial_aa_state']
        for label in ('Height-derived normals (off = cheaper)', 'Gather validation (may be cheaper)',
                      'R16 height textures (less GPU memory)'):
            assert metadata['controls'][label] is True, (label, 'must stay enabled')
        result['controls'] = metadata['controls']
        result['scene_sha256'] = metadata['scene_sha256']
        result['effective_tower_configuration_sha256'] = metadata['effective_tower_configuration_sha256']
        result['capture_sha256'] = hashlib.sha256(Path(config['capture']).read_bytes()).hexdigest()
        assert developer_mode_enabled(), ('Windows Developer Mode is off. Do not call FetchCounters: '
            'D3D12 SetStablePowerState would remove the replay device. Use the explicit temporary-mode wrapper.')
        # This affects only this analysis process. Do not call SaveConfigSettings.
        feedback = rd.SetConfigSetting('D3D12.BindlessFeedback')
        assert feedback is not None
        original_feedback = feedback.AsBool()
        feedback.data.basic.b = False
        assert not feedback.AsBool()
        result['bindless_feedback'] = {'original': original_feedback, 'active': False, 'scope': 'process-local'}
        result['renderdoc_log'] = rd.GetLogFile()
        cap = rd.OpenCaptureFile()
        assert cap.OpenFile(config['capture'], '', None) == rd.ResultCode.Succeeded
        options = rd.ReplayOptions()
        options.forceGPUVendor = rd.GPUVendor.AMD
        options.forceGPUDeviceID = int(config.get('gpu_device_id', 0x1638))
        status, controller = cap.OpenCapture(options, None)
        assert status == rd.ResultCode.Succeeded, str(status)
        healthy()
        props = controller.GetAPIProperties()
        result['renderer'] = {'vendor': str(props.vendor), 'api': str(props.localRenderer), 'degraded': props.degraded}
        structured = controller.GetStructuredFile()
        actions = {}

        def walk(items, parents=()):
            for action in items:
                name = action.GetName(structured)
                actions[action.eventId] = {'event': action.eventId, 'name': name,
                    'path': '/'.join(parents + (name,)), 'flags': str(action.flags),
                    'indices': action.numIndices, 'instances': action.numInstances}
                walk(action.children, parents + (name,))

        walk(controller.GetRootActions())
        available = controller.EnumerateCounters()
        result['available_counters'] = [{'id': int(c), 'name': controller.DescribeCounter(c).name} for c in available]
        counters = [rd.GPUCounter.EventGPUDuration, rd.GPUCounter.PSInvocations,
                    rd.GPUCounter.VSInvocations, rd.GPUCounter.CSInvocations, rd.GPUCounter.SamplesPassed]
        counters = [c for c in counters if c in available]
        assert rd.GPUCounter.EventGPUDuration in counters and rd.GPUCounter.PSInvocations in counters
        descriptions = {int(c): controller.DescribeCounter(c) for c in counters}
        result['counter_descriptions'] = [{'id': i, 'name': d.name, 'type': str(d.resultType),
            'bytes': d.resultByteWidth, 'unit': str(d.unit)} for i, d in descriptions.items()]
        for repeat in range(repeats + 1):
            save('counter-warmup' if repeat == 0 else 'counter-round-' + str(repeat))
            samples = controller.FetchCounters(counters)
            healthy()
            assert samples, 'No counter results'
            events = {}
            for sample in samples:
                desc = descriptions[int(sample.counter)]
                events.setdefault(sample.eventId, {})[desc.name] = counter_value(sample, desc)
            if repeat:
                result['rounds'].append({'repeat': repeat, 'events': events})
            else:
                result['warmup_events'] = len(events)
        result['counter_collection_complete'] = True
        save('counters-complete')
        ranked = []
        for event, action in actions.items():
            times = [r['events'].get(event, {}).get('GPU Duration') for r in result['rounds']]
            if all(t is not None for t in times):
                assert all(t >= 0 for t in times)
                action['duration_ms'] = [t * 1000 for t in times]
                action['median_ms'] = statistics.median(action['duration_ms'])
                action['min_ms'] = min(action['duration_ms'])
                action['max_ms'] = max(action['duration_ms'])
                action['ps_invocations'] = [r['events'][event]['PS Invocations'] for r in result['rounds']]
                ranked.append(action)
        ranked.sort(key=lambda action: action['median_ms'], reverse=True)
        result['timed_events'] = ranked
        shader_folder = output / 'shaders'
        shader_folder.mkdir(exist_ok=True)
        # Mapping, bytecode exports and inspection happen after counter intervals.
        for action in ranked:
            if not any(flag in action['flags'] for flag in ('Drawcall', 'Dispatch')):
                continue
            result['active_event'] = action['event']
            save('mapping-shaders')
            controller.SetFrameEvent(action['event'], False)
            healthy()
            pipe = controller.GetPipelineState()
            stages = [rd.ShaderStage.Compute] if 'Dispatch' in action['flags'] else [rd.ShaderStage.Vertex, rd.ShaderStage.Pixel]
            action['shaders'] = {}
            for stage in stages:
                shader = str(pipe.GetShader(stage))
                action['shaders'][str(stage)] = shader
                if shader in result['shaders'] or shader == 'ResourceId::0':
                    continue
                reflection = pipe.GetShaderReflection(stage)
                if reflection is None:
                    continue
                stem = shader.split('::')[-1] + '-' + str(stage).split('.')[-1]
                raw = bytes(reflection.rawBytes)
                if config.get('shader_export', True):
                    (shader_folder / (stem + '.dxbc')).write_bytes(raw)
                    pipeline = pipe.GetComputePipelineObject() if stage == rd.ShaderStage.Compute else pipe.GetGraphicsPipelineObject()
                    disassembly = controller.DisassembleShader(pipeline, reflection, '')
                    (shader_folder / (stem + '.txt')).write_text(disassembly)
                result['shaders'][shader] = {'stem': stem, 'stage': str(stage), 'entry': reflection.entryPoint,
                    'encoding': str(reflection.encoding), 'sha256': hashlib.sha256(raw).hexdigest(),
                    'read_only_resources': [r.name for r in reflection.readOnlyResources],
                    'read_write_resources': [r.name for r in reflection.readWriteResources],
                    'constant_blocks': [b.name for b in reflection.constantBlocks],
                    'debug_files': [f.filename for f in reflection.debugInfo.files]}
            result['draws'].append(action)
        result['message_severities'] = dict(collections.Counter(str(m.severity) for m in controller.GetDebugMessages()))
        healthy()
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
        if feedback is not None and original_feedback is not None:
            feedback.data.basic.b = original_feedback
        # Successful replay logs can be deleted when qrenderdoc exits.
        log = result.get('renderdoc_log')
        if log and Path(log).is_file():
            shutil.copyfile(log, str(output / 'renderdoc.log'))
        (output / 'profile.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


try:
    configuration = json.loads(Path(os.environ['SILPOM_RENDERDOC_CONFIG']).read_text(encoding='utf-8-sig'))
    if 'profiles' in configuration:
        completed = []
        for item in configuration['profiles']:
            current = profile(item)
            completed.append({'output': item['output'], 'passed': current['passed'],
                'sample_count': current.get('sample_count'),
                'capture_sha256': current.get('capture_sha256'),
                'final_fatal_status': current.get('final_fatal_status')})
            if not current['passed']:
                break
        output = Path(configuration['output'])
        output.mkdir(parents=True, exist_ok=True)
        (output / 'profile.json').write_text(json.dumps({
            'passed': bool(completed) and len(completed) == len(configuration['profiles'])
                and all(item['passed'] for item in completed),
            'profiles': completed}, indent=2) + '\n')
    else:
        profile(configuration)
finally:
    # qrenderdoc treats SystemExit specially and exits before opening its UI.
    sys.exit(0)

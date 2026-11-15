"""Summarize three or more 120-frame captures per frozen tower camera."""
import argparse
from collections import defaultdict
import json
from pathlib import Path

from analyze_planar_ab import distribution, summarize_capture


VIEWS = ('exterior', 'lower-facing', 'lower-glancing', 'upper-glancing')


def analyze(folder):
    status = json.loads((folder / 'results.json').read_text())
    assert status['passed'] and status['restored'] and status['timing_only']
    assert status['saved_level_unchanged'] and status['vue_live_link_unchanged']
    assert status['controls'].get('Height-derived normals (off = cheaper)') is True,'Height-derived normals must remain enabled'
    report = {
        'scene_sha256': status['scene_sha256'],
        'saved_controls': status['controls'],
        'requested_overrides': status.get('requested_overrides', {}),
        'saved_height_derived_normals': status.get('saved_height_derived_normals'),
        'tower_configuration_sha256': status['tower_configuration_sha256'],
        'effective_tower_configuration_sha256': status['effective_tower_configuration_sha256'],
        'cpu_frame_time_note': 'RHI frame-to-frame period at timestamp collection, including waits; delayed GPU readbacks are not paired CPU/GPU frames. Zero means unavailable. Capture wall time is not CPU frame time.',
        'graphics_span_note': 'First-to-last graphics-pass timestamp span, including queue gaps; not GPU busy time or FPS.',
        'pass_time_note': 'Leaf timestamp durations. Parent passes are excluded from pass sums.',
        'views': {},
    }
    devices = set()
    for view in VIEWS:
        files = sorted(folder.glob(f'gpu-{view}-*.json'))
        assert len(files) >= 3, (view, 'At least three captures required')
        pooled = defaultdict(list)
        captures = []
        for path in files:
            raw = json.loads(path.read_text())
            assert raw['frames'] == raw['requested'] == 120, path
            assert raw['depth_attachment'] == {'width': 1280, 'height': 720, 'msaa': 1}, path
            devices.add(raw['hardware'])
            checked = summarize_capture(raw)
            spans = raw['graphics_span_ns']
            assert len(spans) == 120 and all(value > 0 for value in spans), path
            pooled['graphics_pass_span'].extend(ns / 1e6 for ns in spans)
            cpu = raw.get('cpu_frame_ms', [])
            assert not cpu or len(cpu) == 120, path
            if cpu and all(value > 0 for value in cpu):
                pooled['cpu_frame_period'].extend(cpu)
            for _, index, _, _, ns in raw['samples']:
                info = raw['passes'][index]
                if not info['parent']:
                    pooled[info['path']].append(ns / 1e6)
            targets = {}
            for metric, suffix in (
                ('hit_generation', '.DepthPrePass.SilPOMHitDepth'),
                ('silpom_forward_shading', '.OpaquePass.SilPOMHitForward'),
            ):
                matches = [name for name in checked['passes'] if name.endswith(suffix)]
                assert len(matches) == 1, (path, metric, matches)
                name = matches[0]
                assert checked['passes'][name]['count'] == 120, (path, 'Missing pass timestamps', name)
                targets[metric] = checked['passes'][name]
            captures.append({'file': path.name, **targets,
                             'graphics_pass_span': checked['graphics_pass_span'],
                             'cpu_frame_period': distribution(cpu) if cpu and all(value > 0 for value in cpu) else None,
                             'partial_passes': checked['partial_passes']})
        metrics = {name: distribution(values) for name, values in pooled.items()}
        repeat_median_ranges = {}
        metrics_with_repeats = ['hit_generation', 'silpom_forward_shading', 'graphics_pass_span']
        if all(capture['cpu_frame_period'] is not None for capture in captures):
            metrics_with_repeats.append('cpu_frame_period')
        for metric in metrics_with_repeats:
            medians = [capture[metric]['median_ms'] for capture in captures]
            repeat_median_ranges[metric] = {'min_ms': min(medians), 'max_ms': max(medians)}
        report['views'][view] = {'captures': captures, 'pooled': metrics,
                                 'repeat_median_ranges': repeat_median_ranges}
    assert len(devices) == 1, ('Rendering device changed', devices)
    report.update(hardware=devices.pop(), resolution=[1280, 720], samples=1)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.folder)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))

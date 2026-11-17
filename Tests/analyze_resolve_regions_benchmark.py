"""Compare interleaved tower resolve GPU timestamp captures at 2x MSAA.

Usage: python analyze_resolve_regions_benchmark.py <folder> --output report.json
Uses only the standard library. GPU pass time is reported independently from
the first-to-last graphics timestamp span; no parent/child pass sums are used.
"""
import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import statistics


TARGETS = {
    'resolve_gpu': '.OpaquePass.SilPOMHitForward',
    'hit_generation_gpu': '.DepthPrePass.SilPOMHitDepth',
}
METRICS = (*TARGETS, 'graphics_span', 'cpu_rhi_period')
EXPECTED_ATTACHMENT = {'width': 1280, 'height': 720, 'msaa': 2}


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def percentile(values, fraction):
    values = sorted(values)
    position = (len(values) - 1) * fraction
    low = int(position)
    return values[low] + (values[min(low + 1, len(values) - 1)] - values[low]) * (position - low)


def distribution(values):
    assert values and all(math.isfinite(value) and value >= 0 for value in values), 'Invalid timing values'
    return {
        'count': len(values), 'median_ms': statistics.median(values),
        'p05_ms': percentile(values, .05), 'p95_ms': percentile(values, .95),
        'min_ms': min(values), 'max_ms': max(values),
    }


def improvement(full, region):
    """Positive saved time/reduction means region scheduling is faster."""
    before, after = full['median_ms'], region['median_ms']
    return {
        'full': full, 'region': region,
        'saved_ms': before - after,
        'reduction_percent': 100 * (before - after) / before if before else None,
        'speedup_x': before / after if after else None,
        'p95_saved_ms': full['p95_ms'] - region['p95_ms'],
        'p95_reduction_percent': 100 * (full['p95_ms'] - region['p95_ms']) / full['p95_ms'] if full['p95_ms'] else None,
    }


def capture_values(raw, path):
    frames = raw['frames']
    assert raw['complete'] and frames == raw['requested'] and frames >= 120, (path, 'Incomplete or short capture')
    assert raw['depth_attachment'] == EXPECTED_ATTACHMENT, (path, 'Wrong viewport/sample count')
    seen, frame_passes = set(), set()
    leaves = defaultdict(list)
    for frame, index, begin, ticks, nanoseconds in raw['samples']:
        assert 0 <= frame < frames and 0 <= index < len(raw['passes']), (path, 'Invalid timestamp index')
        assert begin > 0 and ticks >= 0 and nanoseconds >= 0, (path, 'Invalid timestamp')
        assert (index, begin) not in seen and (frame, index) not in frame_passes, (path, 'Duplicate/stale timestamp')
        seen.add((index, begin)); frame_passes.add((frame, index))
        info = raw['passes'][index]
        if info['parent']:
            continue
        # Readbacks/inspection add work and must never enter this timing run.
        assert not any(word in info['path'].lower() for word in ('silpominspect', 'silpomuvusage', 'readback', 'framecapture')), \
            (path, 'Measured inspection/readback pass', info['path'])
        leaves[info['path']].append(nanoseconds / 1e6)
    metrics = {}
    for label, suffix in TARGETS.items():
        matches = [name for name in leaves if name.endswith(suffix)]
        assert len(matches) == 1, (path, label, 'Missing or ambiguous pass', matches)
        values = leaves[matches[0]]
        assert len(values) == frames and all(value > 0 for value in values), (path, label, 'Missing timestamps')
        metrics[label] = values
    spans = raw['graphics_span_ns']
    assert len(spans) == frames and all(value > 0 for value in spans), (path, 'Missing graphics spans')
    metrics['graphics_span'] = [value / 1e6 for value in spans]
    cpu = raw.get('cpu_frame_ms', [])
    assert len(cpu) == frames and all(value > 0 for value in cpu), (path, 'Missing RHI frame periods')
    metrics['cpu_rhi_period'] = cpu
    return metrics, leaves


def analyze(folder):
    state_path = folder / 'results.json'
    state = read(state_path)
    assert state['passed'] and state['restored'] and state['saved_level_unchanged'], 'Run did not complete and restore'
    assert state['benchmark_performed'] and state['timing_only'], 'Not a timing-only benchmark'
    assert state['samples'] == 2 and state['viewport'] == [1280, 720], 'Requires 1280x720 2x MSAA'
    assert state['controls']['Height-derived normals (off = cheaper)'] is True, 'Normals must remain on in both modes'
    config_hash = state['effective_tower_configuration_sha256']
    assert config_hash and state['scene_sha256'], 'Missing scene/configuration provenance'
    repeats = int(state['repeats'])
    assert repeats >= 3, 'At least three paired captures per mode/view required'
    declared_views = list(state['views'])
    assert 'exterior' in declared_views and any(view != 'exterior' for view in declared_views), 'Exterior and interior views required'
    grouped, devices, captures = defaultdict(dict), set(), []
    files = set()
    for ordinal, run in enumerate(state['runs']):
        view, repeat, mode = run['view'], int(run['repeat']), run['mode']
        assert view in declared_views and mode in ('full', 'region'), ('Unknown view/mode', run)
        assert 1 <= repeat <= repeats, ('Invalid repeat index', run)
        key = (view, repeat, mode)
        assert key not in grouped, ('Duplicate capture', key)
        relative = Path(run['file'])
        assert not relative.is_absolute() and '..' not in relative.parts, ('Capture outside output folder', relative)
        path = folder / relative
        assert path not in files, ('Duplicate raw file', path)
        files.add(path)
        raw = read(path)
        if 'sha256' in run:
            assert run['sha256'] == sha256(path), (path, 'Raw timestamp file hash changed')
        metadata = raw['capture_metadata']
        assert metadata == run['capture_metadata'], (path, 'Manifest/raw metadata mismatch')
        assert metadata['mode'] == mode and metadata['view'] == view and int(metadata['repeat']) == repeat, (path, 'Wrong run identity')
        assert metadata['resolve_regions'] is (mode == 'region'), (path, 'Wrong scheduler state')
        assert metadata['aa_state'] == [2, 0, 0] and metadata['viewport'] == [1280, 720], (path, 'AA/viewport changed')
        assert metadata['effective_tower_configuration_sha256'] == config_hash, (path, 'Material/configuration changed')
        assert metadata['inspection'] is False, (path, 'Inspection active during timing')
        assert not metadata.get('readback_during_timing', False), (path, 'Readback active during timing')
        assert metadata.get('timing_only', True), (path, 'Not timing only')
        if 'frames_per_capture' in state:
            assert raw['frames'] == state['frames_per_capture'], (path, 'Capture frame count changed')
        metrics, leaves = capture_values(raw, path)
        devices.add(raw['hardware'])
        record = {
            'file': run['file'], 'sha256': sha256(path), 'ordinal': ordinal,
            'view': view, 'repeat': repeat, 'mode': mode, 'frames': raw['frames'],
            'hardware': raw['hardware'], 'metrics': {name: distribution(values) for name, values in metrics.items()},
            'capture_metadata': metadata,
            'partial_leaf_passes': [name for name, values in leaves.items() if len(values) != raw['frames']],
        }
        grouped[key] = (record, metrics, leaves)
        captures.append(record)
    assert len(devices) == 1 and next(iter(devices)), ('Rendering device changed', devices)
    assert len(grouped) == len(declared_views) * repeats * 2, 'Missing paired captures'
    report = {
        'passed': False, 'units': 'milliseconds', 'hardware': next(iter(devices)),
        'samples': 2, 'viewport': [1280, 720], 'repeats_per_mode_per_view': repeats,
        'scene_sha256': state['scene_sha256'], 'effective_tower_configuration_sha256': config_hash,
        'controls': state['controls'], 'view_poses': state['views'],
        'source_sha256': state.get('source_sha256', {}), 'capture_orders': state.get('orders', []),
        'results_file': str(state_path), 'results_sha256': sha256(state_path),
        'method': 'Native RHI GPU timestamps; interleaved full-screen/region captures in one Editor session',
        'graphics_span_note': 'First-to-last graphics-pass timestamp interval including queue gaps; not GPU busy time, total leaf work or FPS.',
        'cpu_rhi_period_note': 'RHI frame-to-frame period at delayed timestamp collection, including waits; not CPU busy time or paired CPU/GPU frames.',
        'statistics_note': 'Pooled frame median/p95 plus paired per-capture medians; frames within one capture are correlated, so no independent-frame confidence interval is claimed.',
        'saved_level_unchanged': True, 'restored': True, 'captures': captures, 'views': {},
    }
    hardware_path = folder / 'hardware-before.json'
    if hardware_path.exists():
        report['hardware_snapshot'] = {'file': str(hardware_path), 'sha256': sha256(hardware_path), 'data': read(hardware_path)}
    for view in declared_views:
        pooled = {mode: defaultdict(list) for mode in ('full', 'region')}
        leaf_pooled = {mode: defaultdict(list) for mode in ('full', 'region')}
        paired = []
        for repeat in range(1, repeats + 1):
            full_record, full_metrics, full_leaves = grouped[(view, repeat, 'full')]
            region_record, region_metrics, region_leaves = grouped[(view, repeat, 'region')]
            for mode, metrics, leaves in (('full', full_metrics, full_leaves), ('region', region_metrics, region_leaves)):
                for name, values in metrics.items(): pooled[mode][name].extend(values)
                for name, values in leaves.items(): leaf_pooled[mode][name].extend(values)
            pair = {
                'repeat': repeat, 'full_file': full_record['file'], 'region_file': region_record['file'],
                'mode_order': ['full', 'region'] if full_record['ordinal'] < region_record['ordinal'] else ['region', 'full'],
                'metrics': {name: improvement(full_record['metrics'][name], region_record['metrics'][name]) for name in METRICS},
            }
            assert abs(full_record['ordinal'] - region_record['ordinal']) == 1, (view, repeat, 'Paired modes were not adjacent')
            paired.append(pair)
        assert len({tuple(pair['mode_order']) for pair in paired}) == 2, (view, 'Only one mode order was measured')
        comparisons = {name: improvement(distribution(pooled['full'][name]), distribution(pooled['region'][name])) for name in METRICS}
        ranges, paired_summary = {}, {}
        for metric in METRICS:
            ranges[metric] = {
                mode: {'min_ms': min(pair['metrics'][metric][mode]['median_ms'] for pair in paired),
                       'max_ms': max(pair['metrics'][metric][mode]['median_ms'] for pair in paired)}
                for mode in ('full', 'region')
            }
            paired_summary[metric] = {}
            for field in ('saved_ms', 'reduction_percent', 'speedup_x'):
                values = [pair['metrics'][metric][field] for pair in paired]
                paired_summary[metric][field] = {'median': statistics.median(values), 'min': min(values), 'max': max(values)}
        leaves = {
            name: improvement(distribution(leaf_pooled['full'][name]), distribution(leaf_pooled['region'][name]))
            for name in sorted(leaf_pooled['full'].keys() & leaf_pooled['region'].keys())
        }
        report['views'][view] = {
            'metrics': comparisons, 'paired_repeat_summary': paired_summary,
            'repeat_median_ranges': ranges, 'paired_repeats': paired,
            'leaf_passes': leaves,
            'unmatched_leaf_passes': {mode: sorted(leaf_pooled[mode].keys() - leaf_pooled['region' if mode == 'full' else 'full'].keys()) for mode in ('full', 'region')},
        }
    report['passed'] = True
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = analyze(args.folder)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('View | resolve full / region ms | resolve speedup | graphics span full / region ms | graphics span speedup')
    for name, view in report['views'].items():
        resolve, span = view['metrics']['resolve_gpu'], view['metrics']['graphics_span']
        print(f"{name} | {resolve['full']['median_ms']:.3f} / {resolve['region']['median_ms']:.3f} | "
              f"{resolve['speedup_x']:.3f}x ({resolve['reduction_percent']:+.2f}%) | "
              f"{span['full']['median_ms']:.3f} / {span['region']['median_ms']:.3f} | "
              f"{span['speedup_x']:.3f}x ({span['reduction_percent']:+.2f}%)")


if __name__ == '__main__':
    main()

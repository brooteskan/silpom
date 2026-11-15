"""Summarize the gameplay benchmark's raw timestamps and material attachments."""
import argparse
import json
from pathlib import Path
import statistics
import struct


def median(values):
    return statistics.median(values) if values else None


def attachment(path):
    data = path.read_bytes()
    assert data[:4] == b'DDS ' and data[84:88] == b'DX10', path
    return struct.unpack_from('<III', data, 12)[0:2], data[148:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    args = parser.parse_args()
    report = {'runs': {}, 'attachment_comparisons': {}}
    grouped = {}
    for phase, folder in (('before', args.before), ('after', args.after)):
        state = json.loads((folder / 'results.json').read_text())
        assert state['passed'] and state['saved_level_unchanged'], state
        report[phase] = state
        for path in sorted(folder.glob('*-gpu.json')):
            data = json.loads(path.read_text())
            assert data['complete'] and data['frames'] == 180 and data['depth_attachment'] == {'width': 1280, 'height': 720, 'msaa': 2}
            stem = path.name.removesuffix('-gpu.json')
            fixture, view, repeat, mode = stem.split('-')
            ids = {i for i, p in enumerate(data['passes']) if p['path'].endswith('.SilPOMHitForward')}
            hit = [s[4] / 1e6 for s in data['samples'] if s[1] in ids]
            span = [v / 1e6 for v in data['graphics_span_ns'] if v]
            cpu = [v for v in data['cpu_frame_ms'] if v]
            record = {'gpu_span_ms': median(span), 'hit_forward_ms': median(hit), 'cpu_frame_period_ms': median(cpu), 'hardware': data['hardware']}
            report['runs'][phase + '/' + stem] = record
            key = '/'.join((phase, fixture, view, mode))
            grouped.setdefault(key, []).append(record)
    report['medians_of_runs'] = {k: {field: median([v[field] for v in rows]) for field in ('gpu_span_ms', 'hit_forward_ms', 'cpu_frame_period_ms')} for k, rows in grouped.items()}
    for fixture in ('saved', 'spread'):
        for view in ('inside', 'outside'):
            _, hit_a = attachment(args.after / f'{fixture}-{view}-0-0-hit.dds')
            _, hit_b = attachment(args.after / f'{fixture}-{view}-1-1-hit.dds')
            stable = [i for i, (a, b) in enumerate(zip(struct.iter_unpack('<IIII', hit_a), struct.iter_unpack('<IIII', hit_b))) if a == b and (a[3] & 3) == 1]
            report.setdefault('hit_comparisons', {})[f'{fixture}/{view}'] = {'identical': hit_a == hit_b, 'stable_valid_pixels': len(stable)}
            for slot in ('diffuse', 'normal'):
                a = args.after / f'{fixture}-{view}-0-0-{slot}.dds'
                b = args.after / f'{fixture}-{view}-1-1-{slot}.dds'
                shape_a, raw_a = attachment(a); shape_b, raw_b = attachment(b)
                assert shape_a == shape_b and len(raw_a) == len(raw_b)
                changed = sum(x != y for x, y in zip(raw_a, raw_b))
                stride = len(raw_a) // (len(hit_a) // 16)
                tower_changed = sum(raw_a[i*stride:(i+1)*stride] != raw_b[i*stride:(i+1)*stride] for i in stable)
                report['attachment_comparisons'][f'{fixture}/{view}/{slot}'] = {'identical': raw_a == raw_b, 'changed_bytes': changed, 'total_bytes': len(raw_a), 'tower_changed_pixels': tower_changed}
                assert hit_a == hit_b and tower_changed == 0, (fixture, view, slot, tower_changed)
    for fixture in ('saved', 'spread'):
        _, raw = attachment(args.after / f'{fixture}-inside-1-1-diffuse.dds')
        _, hidden = attachment(args.after / f'{fixture}-inside-no-paint-diffuse.dds')
        report.setdefault('visible_paint', {})[fixture] = {'changed_bytes': sum(x != y for x, y in zip(raw, hidden))}
    out = args.after / 'analysis.json'
    out.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('medians_of_runs', 'attachment_comparisons', 'visible_paint')}, indent=2))


if __name__ == '__main__': main()

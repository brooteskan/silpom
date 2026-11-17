"""Compare focused 2x tower full/disjoint/full repair readbacks, without timing.

Usage: python test_tower_repair_regions.py <capture-folder> [report.json]
All hit records and depths must match bit for bit. Diffuse, specular and normal
samples must match on every owned sample. A repeated full-screen control must
also match, and trace evidence must prove the regional/multiple-draw path ran.
"""
import hashlib
import json
from pathlib import Path
import sys

import numpy as np

from test_tower_resolve_regions import read_atlas


VIEWS = ('exterior', 'lower-facing', 'lower-glancing', 'upper-glancing')
FORMATS = (
    ('hit', 3, '<u4', 4), ('depth', 41, '<u4', 1),
    ('diffuse', 10, '<u2', 4), ('specular', 10, '<u2', 4), ('normal', 10, '<u2', 4),
)


def compare(reference, candidate, mask):
    different = reference != candidate
    return {
        'different_samples': int(np.count_nonzero(np.any(different, axis=-1) & mask)),
        'different_components': int(np.count_nonzero(different & mask[..., None])),
    }


def digest(values):
    return hashlib.sha256(values.tobytes()).hexdigest()


def validate(folder):
    state = json.loads((folder / 'results.json').read_text())
    report = {'passed': False, 'benchmark_performed': False, 'samples': 2,
              'viewport': [1280, 720], 'comparison': 'full_screen_vs_disjoint_repair',
              'comparison_policy': 'bitwise hit/depth all samples; bitwise shading owned samples',
              'capture_controls': state.get('controls', {}), 'cases': [], 'failures': []}
    failures = report['failures']
    for flag in ('passed', 'restored', 'saved_level_unchanged'):
        if state.get(flag) is not True:
            failures.append(f'Capture state {flag} is not true')
    if state.get('benchmark_performed') is not False:
        failures.append('Capture did not declare benchmarking disabled')
    groups = {}
    for run in state.get('runs', []):
        key = (run['samples'], run['view'])
        group = groups.setdefault(key, {})
        if run['mode'] in group:
            failures.append(f'Duplicate capture: {key}, {run["mode"]}')
        group[run['mode']] = run
    if set(groups) != {(2, name) for name in VIEWS}:
        failures.append(f'Expected exactly the four 2x tower views, found {list(groups)}')
    traces = state.get('repair_traces', {})
    exercised_multiple = False
    for name in VIEWS:
        runs = groups.get((2, name), {})
        case = {'view': name, 'samples': 2, 'width': 1280, 'height': 720,
                'comparisons': {}, 'trace_evidence': traces.get(name, [])}
        report['cases'].append(case)
        regional = [trace for trace in traces.get(name, [])
                    if trace.get('mode') == 1 and trace.get('paired') == 1
                    and trace.get('producers', 0) > 0 and trace.get('rectangles', 0) > 0]
        case['regional_path_observed'] = bool(regional)
        if not regional:
            failures.append(f'{name}: no active regional repair trace')
        exercised_multiple |= any(trace['rectangles'] > 1 for trace in regional)
        if set(runs) != {'full', 'region', 'control'}:
            failures.append(f'{name}: expected full/region/control captures')
            continue
        if any((run['samples'], run['width'], run['height']) != (2, 1280, 720) for run in runs.values()):
            failures.append(f'{name}: inconsistent sample count or viewport')
            continue
        try:
            captures = {mode: {
                suffix: read_atlas(folder / f'{run["stem"]}-{suffix}.dds',
                                   1280, 720, 2, fmt, dtype, channels)
                for suffix, fmt, dtype, channels in FORMATS}
                for mode, run in runs.items()}
        except Exception as error:
            case['read_error'] = str(error)
            failures.append(f'{name}: attachment read failed: {error}')
            continue
        full = captures['full']
        owners = full['hit'][..., 3] >> 2
        owned = owners != 0
        all_samples = np.ones_like(owned)
        case['total_samples'] = int(owned.size)
        case['owned_samples'] = int(np.count_nonzero(owned))
        case['owned_status_counts'] = {str(status): int(np.count_nonzero(
            owned & ((full['hit'][..., 3] & 3) == status))) for status in range(4)}
        if case['owned_samples'] <= owned.size * .005:
            failures.append(f'{name}: insufficient tower coverage')
        for suffix, reference in full.items():
            mask = all_samples if suffix in ('hit', 'depth') else owned
            checks = {'compared_samples': int(np.count_nonzero(mask)), 'full_sha256': digest(reference)}
            for mode in ('region', 'control'):
                candidate = captures[mode][suffix]
                checks[mode] = compare(reference, candidate, mask)
                checks[f'{mode}_sha256'] = digest(candidate)
                if checks[mode]['different_samples']:
                    failures.append(f'{name}: {suffix} {mode} differs at '
                                    f'{checks[mode]["different_samples"]} samples')
            # Checks apply to the rendered tower, including diagnostic statuses.
            if suffix != 'hit':
                dtype = '<f4' if suffix == 'depth' else '<f2'
                checks['nonfinite_owned_components'] = {
                    mode: int(np.count_nonzero(~np.isfinite(data[suffix].view(dtype)[owned])))
                    for mode, data in captures.items()}
                if any(checks['nonfinite_owned_components'].values()):
                    failures.append(f'{name}: {suffix} has nonfinite owned output')
            case['comparisons'][suffix] = checks
        case['hit_fields'] = {}
        for mode in ('region', 'control'):
            candidate = captures[mode]['hit']
            case['hit_fields'][mode] = {
                'ownership_different_samples': int(np.count_nonzero((candidate[..., 3] >> 2) != owners)),
                'status_different_samples': int(np.count_nonzero((candidate[..., 3] & 3) != (full['hit'][..., 3] & 3))),
                'uv_different_samples': int(np.count_nonzero(np.any(candidate[..., :2] != full['hit'][..., :2], axis=-1))),
                'record_z_different_samples': int(np.count_nonzero(candidate[..., 2] != full['hit'][..., 2])),
            }
    report['multiple_rectangles_observed'] = exercised_multiple
    if not exercised_multiple:
        failures.append('No tested view exercised more than one repair rectangle')
    report['owned_samples'] = sum(case.get('owned_samples', 0) for case in report['cases'])
    report['passed'] = not failures
    return report


if __name__ == '__main__':
    folder = Path(sys.argv[1])
    destination = Path(sys.argv[2]) if len(sys.argv) > 2 else folder / 'comparison.json'
    try:
        output = validate(folder)
    except Exception as error:
        output = {'passed': False, 'benchmark_performed': False, 'failures': [str(error)]}
    destination.write_text(json.dumps(output, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(output, indent=2))
    raise SystemExit(0 if output['passed'] else 1)

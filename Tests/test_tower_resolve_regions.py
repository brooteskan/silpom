"""Validate full/region/full per-sample readbacks from the isolated Editor.

Usage: python test_tower_resolve_regions.py <capture-folder> [report.json]
Requires numpy. This is a pixel equality check, not a performance measurement.
The repeated full-screen control must be stable before differences are attributed
to region scheduling; ownership/depth retain their original integer bit patterns.
"""
import hashlib
import json
from pathlib import Path
import struct
import sys

import numpy as np


def read_atlas(path, width, height, samples, format_id, dtype, channels):
    raw = path.read_bytes()
    assert raw[:4] == b'DDS ' and raw[84:88] == b'DX10', str(path)
    actual_height, actual_width = struct.unpack_from('<II', raw, 12)
    actual_format, = struct.unpack_from('<I', raw, 128)
    assert (actual_width, actual_height, actual_format) == (width * 4, height, format_id), str(path)
    data = np.frombuffer(raw, dtype=dtype, offset=148).reshape(height, width, 4, channels)
    return data[:, :, :samples, :]


def validate(folder):
    result = json.loads((folder / 'results.json').read_text())
    assert result['passed'] and result['restored'] and result['saved_level_unchanged'], result
    groups = {}
    for run in result['runs']:
        groups.setdefault((run['samples'], run['view']), {})[run['mode']] = run
    assert groups, 'No captures'
    report = {'passed': False, 'benchmark_performed': False, 'cases': []}
    for (samples, name), runs in groups.items():
        assert set(runs) == {'full', 'region', 'control'}, (samples, name)
        run = runs['full']
        width, height = run['width'], run['height']
        captures = {}
        for mode, capture in runs.items():
            captures[mode] = {}
            for suffix, fmt, dtype, channels in (
                ('hit', 3, '<u4', 4), ('depth', 41, '<u4', 1),
                ('diffuse', 10, '<u2', 4), ('specular', 10, '<u2', 4), ('normal', 10, '<u2', 4),
            ):
                captures[mode][suffix] = read_atlas(folder / f"{capture['stem']}-{suffix}.dds",
                                                  width, height, samples, fmt, dtype, channels)
        full, region, control = (captures[mode] for mode in ('full', 'region', 'control'))
        owners = full['hit'][..., 3] >> 2
        owned = owners != 0
        owned_count = int(np.count_nonzero(owned))
        assert owned_count > width * height * samples * .005, (name, 'Missing tower coverage')
        case = {'samples': samples, 'view': name, 'width': width, 'height': height,
                'owned_samples': owned_count, 'comparisons': {}}
        for suffix in full:
            # Hit/depth compare every sample, including sky and diagnostics.
            # Shaded attachments compare all owned samples, including failures.
            mask = np.ones_like(owned) if suffix in ('hit', 'depth') else owned
            delta = np.any(full[suffix] != region[suffix], axis=-1) & mask
            noise = np.any(full[suffix] != control[suffix], axis=-1) & mask
            case['comparisons'][suffix] = {
                'region_different_samples': int(np.count_nonzero(delta)),
                'control_different_samples': int(np.count_nonzero(noise)),
                'full_sha256': hashlib.sha256(full[suffix].tobytes()).hexdigest(),
                'region_sha256': hashlib.sha256(region[suffix].tobytes()).hexdigest(),
            }
            assert not np.any(noise), (name, samples, suffix, 'Unstable full-screen control', case)
            assert not np.any(delta), (name, samples, suffix, 'Region differs from stable full-screen draw', case)
        report['cases'].append(case)
    report['passed'] = True
    return report


if __name__ == '__main__':
    output = validate(Path(sys.argv[1]))
    if len(sys.argv) > 2:
        Path(sys.argv[2]).write_text(json.dumps(output, indent=2) + '\n')
    print(json.dumps(output, indent=2))

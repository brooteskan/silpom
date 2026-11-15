"""Check that 2x gameplay recovery closes wall gaps without changing 1x hits."""
import json
from pathlib import Path
import struct
import sys
import numpy as np


def records(path):
    raw = path.read_bytes()
    assert raw[:4] == b'DDS ' and raw[84:88] == b'DX10'
    assert struct.unpack_from('<I', raw, 128)[0] == 3  # R32G32B32A32_UINT
    height, width = struct.unpack_from('<II', raw, 12)
    return np.frombuffer(raw, '<u4', offset=148).reshape(-1, height, width, 4)[0].copy()


def check(before, after):
    a, b = [json.loads((root/'results.json').read_text()) for root in (before, after)]
    assert all(r['passed'] and r['saved_level_unchanged'] for r in (a, b))
    assert a['level_sha256'] == b['level_sha256']
    result = {'passed': True, 'views': {}}
    for name in ('inside', 'outside'):
        old = records(before/f'msaa2-{name}-hits.dds')
        new = records(after/f'msaa2-{name}-hits.dds')
        reference = records(before/f'smaa1-{name}-hits.dds')
        current = records(after/f'smaa1-{name}-hits.dds')
        assert np.array_equal(reference[..., :3], current[..., :3]), 'Single-sample surface changed'
        assert np.array_equal(reference[..., 3]&3, current[..., 3]&3)
        wall = ((reference[..., 3]&3) == 1) & ((reference[..., 2]&0x40000000) != 0)
        interior = wall.copy()
        # Exclude genuine silhouettes, doorways and stairs from the gap metric.
        for y in range(-2, 3):
            for x in range(-2, 3):
                interior &= np.roll(wall, (y, x), axis=(0, 1))
        interior[:2] = False; interior[-2:] = False
        interior[:, :2] = False; interior[:, -2:] = False
        old_missing = int((interior & ((old[..., 3]&3) != 1)).sum())
        new_missing = int((interior & ((new[..., 3]&3) != 1)).sum())
        recovered = int((interior & ((old[..., 3]&3) != 1) & ((new[..., 3]&3) == 1)).sum())
        assert new_missing <= old_missing, (name, old_missing, new_missing)
        assert int(wall.sum()) > 1000, 'Camera did not exercise a wall'
        result['views'][name] = {'interior_wall_pixels': int(interior.sum()),
            'missing_before': old_missing, 'missing_after': new_missing, 'recovered': recovered,
            'single_sample_hits_unchanged': True}
        depth_path = after/f'msaa2-{name}-depth.dds'
        if depth_path.exists():
            raw_depth = depth_path.read_bytes()
            assert struct.unpack_from('<I', raw_depth, 128)[0] == 41
            depth_bits = np.frombuffer(raw_depth, '<u4', offset=148).reshape(new.shape[:2])
            backup = ((new[..., 3]&3) == 1) & ((new[..., 2]&0xc0000000) == 0xc0000000)
            mismatches = int((backup & ((new[..., 2]&0x3fffffff) != depth_bits)).sum())
            result['views'][name]['backup_depth_mismatches'] = mismatches
            assert mismatches == 0, (name, 'Backup depth equality leaves covered samples unshaded', mismatches)
    assert sum(v['recovered'] for v in result['views'].values()) > 20, 'Fixture did not exercise the reported gaps'
    (after/'coverage-validation.json').write_text(json.dumps(result, indent=2))
    return result


if __name__ == '__main__':
    print(json.dumps(check(Path(sys.argv[1]), Path(sys.argv[2])), indent=2))

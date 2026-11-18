"""Validate seven sample atlases and compare the frozen pre-gradient tower.

Usage: python test_tower_gradient_context.py <capture-folder> [report.json]
       python test_tower_gradient_context.py --self-test
Requires numpy. Readbacks never measure performance. The frozen baseline must
have the same level, effective material controls, views, viewport and 2x MSAA.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

import numpy as np


VIEWS = ('exterior', 'lower-facing', 'lower-glancing', 'upper-glancing')
DEFAULT_LEGACY = (Path(__file__).resolve().parents[3] / 'user' /
                  'silpom-multi-rect-2x-20261116/quality')
FORMATS = {
    'hit': (3, '<u4', 4), 'depth': (41, '<u4', 1),
    'diffuse': (10, '<u2', 4), 'specular': (10, '<u2', 4), 'normal': (10, '<u2', 4),
    'uv-gradients': (2, '<u4', 4), 'depth-gradients': (2, '<u4', 4),
}
REPRESENTATIONS = {1: 'relief', 2: 'base', 3: 'shell', 4: 'backup',
                   5: 'morph', 6: 'proxy', 7: 'proxy-depth', 11: 'diagnostic'}
REPAIRS = {0: 'producer', 256: 'copied-donor', 512: 'arithmetic-bridge',
           768: 'depth-weighted-bridge'}


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def sha(data):
    return hashlib.sha256(data.tobytes()).hexdigest()


def capture_path(folder, stem, suffix):
    assert Path(stem).name == stem and '/' not in stem and '\\' not in stem, stem
    return folder / f'{stem}-{suffix}.dds'


def read_atlas(path, width, height, samples, format_id, dtype, channels):
    raw = path.read_bytes()
    assert len(raw) >= 148 and raw[:4] == b'DDS ' and raw[84:88] == b'DX10', str(path)
    actual_height, actual_width = struct.unpack_from('<II', raw, 12)
    actual_format, = struct.unpack_from('<I', raw, 128)
    assert 1 <= samples <= 4, samples
    assert (actual_width, actual_height, actual_format) == (width * 4, height, format_id), str(path)
    expected = 148 + width * 4 * height * channels * np.dtype(dtype).itemsize
    assert len(raw) == expected, (str(path), 'Wrong DDS payload length', len(raw), expected)
    # Compact the active slots so one case does not retain all unused sample slots.
    return np.frombuffer(raw, dtype=dtype, offset=148).reshape(
        height, width, 4, channels)[:, :, :samples, :].copy()


def load(folder, run, suffix):
    return read_atlas(capture_path(folder, run['stem'], suffix), run['width'], run['height'],
                      run['samples'], *FORMATS[suffix])


def compare(a, b, mask):
    assert a.shape == b.shape, (a.shape, b.shape)
    different = np.any(a != b, axis=-1) & mask
    return {'different_samples': int(np.count_nonzero(different)),
            'compared_samples': int(np.count_nonzero(mask)),
            'reference_sha256': sha(a), 'candidate_sha256': sha(b)}


def gradient_summary(hit, uv_bits, depth_bits):
    owners = hit[..., 3] >> 2
    owned = owners != 0
    assert np.any(owned), 'Missing tower samples'
    uv = uv_bits.view('<f4')
    depth = depth_bits.view('<f4')
    assert np.all(np.isfinite(uv[owned])) and np.all(np.isfinite(depth[owned])), 'Non-finite owned gradient'
    assert np.all(np.abs(uv[owned]) <= 1e30) and np.all(np.abs(depth[owned]) <= 1e30), 'Unbounded owned gradient'
    representation = depth[..., 2][owned]
    validity = depth[..., 3][owned]
    assert np.all(representation == np.floor(representation)), 'Fractional representation metadata'
    assert np.all(np.isin(validity, (1, 2))), 'Owned context is invalid or has unknown validity'
    encoded = representation.astype(np.int64)
    base, repair = encoded & 255, encoded & ~255
    assert np.all(np.isin(base, tuple(REPRESENTATIONS))), 'Unknown representation'
    assert np.all(np.isin(repair, tuple(REPAIRS))), 'Unknown repair provenance'
    coarse = validity == 2
    assert np.all(np.any(uv[owned][coarse] != 0, axis=-1)), 'Coarse context has zero UV footprint'
    unowned = ~owned
    assert np.all(uv_bits[unowned] == 0) and np.all(depth_bits[unowned] == 0), 'Unowned sidecar was not cleared'
    rep_counts = {name: int(np.count_nonzero(base == value)) for value, name in REPRESENTATIONS.items()}
    conditioned = {name: int(np.count_nonzero((base == value) & (validity == 1)))
                   for value, name in REPRESENTATIONS.items()}
    repair_counts = {name: int(np.count_nonzero(repair == value)) for value, name in REPAIRS.items()}
    validity_counts = {'valid': int(np.count_nonzero(validity == 1)), 'coarse': int(np.count_nonzero(coarse))}
    both = (owners[..., 0] != 0) & (owners[..., 1] != 0)
    mixed = both & (owners[..., 0] != owners[..., 1])
    same = both & (owners[..., 0] == owners[..., 1])
    different_footprints = (np.any(uv_bits[..., 0, :] != uv_bits[..., 1, :], axis=-1) |
                            np.any(depth_bits[..., 0, :] != depth_bits[..., 1, :], axis=-1))
    return {'owned_samples': int(np.count_nonzero(owned)), 'representations': rep_counts,
            'conditioned_representations': conditioned,
            'repair_provenance': repair_counts, 'validity': validity_counts,
            'mixed_owned_owner_pixels': int(np.count_nonzero(mixed)),
            'coverage_edge_pixels': int(np.count_nonzero(owned[..., 0] != owned[..., 1])),
            'same_owner_different_gradient_pixels': int(np.count_nonzero(same & different_footprints)),
            'max_abs_source_uv_derivative': float(np.max(np.abs(uv[owned]))),
            'max_abs_depth_derivative': float(np.max(np.abs(depth[..., :2][owned])))}


def shading_delta(a_bits, b_bits, owned):
    # Output attachments are half floats. Report numerical correction magnitude;
    # equality with the derivative-dependent historical shading is not required.
    a, b = a_bits.view('<f2')[owned, :3].astype(np.float64), b_bits.view('<f2')[owned, :3].astype(np.float64)
    assert np.all(np.isfinite(a)) and np.all(np.isfinite(b)), 'Non-finite shading attachment'
    absolute = np.abs(b - a)
    return {'different_owned_samples': int(np.count_nonzero(np.any(a_bits != b_bits, axis=-1) & owned)),
            'owned_samples': int(np.count_nonzero(owned)), 'rgb_mean_abs': float(np.mean(absolute)),
            'rgb_p95_abs': float(np.percentile(absolute, 95)), 'rgb_max_abs': float(np.max(absolute)),
            'rgb_rmse': float(np.sqrt(np.mean((b - a) ** 2))),
            'rgb_mean_change': np.mean(b - a, axis=0).tolist()}


def compare_paint_generation(pristine_hit, painted_hit, pristine_depth, painted_depth,
                             canonical_face, original_face_id, bound_face_id):
    """Use the declared canonical-face mapping, never fit an offset to pixels."""
    assert canonical_face and 0 < original_face_id < (1 << 30) and 0 < bound_face_id < (1 << 30)
    assert original_face_id == int(original_face_id) and bound_face_id == int(bound_face_id), 'Fractional declared face ID'
    assert pristine_hit.shape == painted_hit.shape
    before = (pristine_hit[..., 3] >> 2).astype(np.int64)
    after = (painted_hit[..., 3] >> 2).astype(np.int64)
    owned = before != 0
    owner_offset = int(bound_face_id) - int(original_face_id)
    expected = before[owned] + owner_offset
    assert np.all((expected > 0) & (expected < (1 << 30))), 'Declared owner mapping exceeds packed owner range'
    assert np.all(after[owned] == expected), 'Owned samples do not follow the declared canonical owner rebase'
    assert np.all(after[~owned] == 0), 'Background owner changed during paint setup'
    remapped = pristine_hit.copy()
    remapped[..., 3][owned] = ((expected << 2) | (pristine_hit[..., 3][owned] & 3)).astype('<u4')
    mask = np.ones_like(owned)
    raw = compare(pristine_hit, painted_hit, mask)
    hit = compare(remapped, painted_hit, mask)
    depth = compare(pristine_depth, painted_depth, mask)
    result = {'raw_hit_different_samples': raw['different_samples'], 'raw_hit_comparison': raw,
              'uv_different_samples': int(np.count_nonzero(np.any(pristine_hit[..., :2] != painted_hit[..., :2], axis=-1))),
              'record_z_different_samples': int(np.count_nonzero(pristine_hit[..., 2] != painted_hit[..., 2])),
              'status_different_samples': int(np.count_nonzero((pristine_hit[..., 3] & 3) != (painted_hit[..., 3] & 3))),
              'hit_after_canonical_owner_remap': hit, 'sample_depth': depth,
              'canonical_owner_remap': {'canonical_face': canonical_face, 'original_hit_face': int(original_face_id),
                                       'bound_hit_face': int(bound_face_id), 'declared_owner_offset': owner_offset,
                                       'owned_samples': int(np.count_nonzero(owned)),
                                       'background_samples': int(np.count_nonzero(~owned)),
                                       'basis': 'Same canonical face in captured face metadata and bound receiver status; offset is not fitted from pixels.'}}
    assert hit['different_samples'] == 0, ('Paint changed UV, recorded z, status or background after declared owner remap', result)
    assert depth['different_samples'] == 0, ('Paint changed sample depth', result)
    return result


def validate_paint_probe(folder, run, full, owned, probe, runs):
    assert probe['passed'] and probe['cleanup'] == {'entity_deleted': True, 'receiver_unbound': True}, probe
    document = probe['document']
    assert len(document['Layers']) == 1 and len(document['Layers'][0]['Composite']) == 2
    assert sum(len(stroke['Points']) for stroke in document['Layers'][0]['Strokes']) == 2
    assert document['ReceiverUv']['Face'] == probe['face']['face']
    assert set(probe['run_status']) == {'full', 'region', 'control'}, 'Missing bound receiver status'
    bound_face_ids = set()
    for status in probe['run_status'].values():
        receiver = status['receiver']
        assert receiver['state'] == 'Receiver UV atlas active' and receiver['ready'] and receiver['mode'] == 2, status
        assert status['fixed_dabs'] == status['live_dabs'] == 2, status
        assert status['render_stats']['unique_projections'] == 4 and status['render_stats']['batches'] > 0, status
        assert receiver['hit_face'] == int(receiver['hit_face']), 'Fractional bound face ID'
        bound_face_ids.add(int(receiver['hit_face']))
    assert len(bound_face_ids) == 1, 'Bound canonical face changed its runtime hit ID between scheduler captures'
    bound_face_id = next(iter(bound_face_ids))
    usage = read_atlas(capture_path(folder, run['stem'], 'usage'), run['width'], run['height'], 2, 3, '<u4', 4)
    for mode in ('region', 'control'):
        other = read_atlas(capture_path(folder, runs[mode]['stem'], 'usage'), run['width'], run['height'], 2, 3, '<u4', 4)
        assert np.array_equal(usage, other), ('Paint usage changed with scheduler', mode)
    atlas = owned & (usage[..., 0] == 2)
    assert np.any(atlas), 'Receiver atlas had no actual per-sample use'
    assert np.all(usage[..., 1][atlas] == bound_face_id), 'Atlas usage differs from the declared bound face'
    assert np.all(usage[..., 1][atlas] == (full['hit'][..., 3] >> 2)[atlas]), 'Paint face differs from winning face'
    assert np.all(usage[..., 2][atlas] == 2) and np.all(usage[..., 3][atlas] != 0), 'Atlas prefix count/owner missing'
    pristine = dict(run, stem=probe['pristine_stem'])
    prefix_only = dict(run, stem=probe['prefix_only_stem'])
    visibility = compare_paint_generation(load(folder, pristine, 'hit'), full['hit'],
        load(folder, pristine, 'depth'), full['depth'], document['ReceiverUv']['Face'],
        probe['face']['hit_face'], bound_face_id)
    contributions = {}
    for suffix in ('diffuse', 'normal'):
        before = load(folder, pristine, suffix)
        fixed = load(folder, prefix_only, suffix)
        contributions[suffix] = {'all_paint': shading_delta(before, full[suffix], owned),
                                  'fixed_atlas': shading_delta(before, fixed, atlas),
                                  'live_forward_composite': shading_delta(fixed, full[suffix], owned)}
    assert contributions['diffuse']['fixed_atlas']['different_owned_samples'] > 0, 'Atlas prefix had no visible contribution'
    assert contributions['diffuse']['live_forward_composite']['different_owned_samples'] > 0, 'Live tail had no visible contribution'
    return {'atlas_samples': int(np.count_nonzero(atlas)),
            'replay_samples': int(np.count_nonzero(owned & (usage[..., 0] == 1))),
            'pristine_visibility': visibility, 'contributions': contributions,
            'consumers_observed': {'receiver_uv_atlas': True, 'forward_paint_composite': True,
                                   'ordinary_noncomposite_decal': False},
            'ordinary_decal_note': 'The painted fixture uses the composite wrapper; ordinary decal helper compilation is separate evidence.'}


def validate(folder, legacy=DEFAULT_LEGACY, hit_uv_roundoff_ulps=0):
    assert 0 <= hit_uv_roundoff_ulps <= 2
    state, old_state = read_json(folder / 'results.json'), read_json(legacy / 'results.json')
    old_report = read_json(legacy / 'report.json')
    for value in (state, old_state):
        assert value['passed'] and value['restored'] and value['saved_level_unchanged'], value
        assert value['samples'] == 2 and value['viewport'] == [1280, 720], 'Requires the fixed 2x fixture'
        assert value['controls']['Height-derived normals (off = cheaper)'] is True
    assert state['temporary_properties_restored'], 'Diagnostic controls were not restored'
    assert state['gradient_contract'] == 'owner-local-first-order-per-raster-pixel'
    for key in ('scene_sha256', 'effective_tower_configuration_sha256', 'controls'):
        assert state[key] == old_state[key], (key, 'Historical fixture settings differ')
    fixture = read_json(Path(__file__).resolve().parent / 'Fixtures/TowerBenchmark/Levels/SilPOMTowerBenchmark/scene.json')
    assert fixture['scene_sha256'] == state['scene_sha256']
    for name in VIEWS:
        assert state['view_poses'][name] == fixture['views'][name], (name, 'Fixed camera changed')
    assert old_report['passed'], 'Frozen baseline validation failed'
    old_cases = {(case['samples'], case['view']): case for case in old_report['cases']}
    old_runs = {(run['view'], run['mode']): run for run in old_state['runs']}
    groups, stems = {}, set()
    for run in state['runs']:
        assert run['samples'] == 2 and (run['width'], run['height']) == (1280, 720)
        assert run['stem'] not in stems, 'Duplicate capture stem'
        stems.add(run['stem'])
        group = groups.setdefault(run['view'], {})
        assert run['mode'] not in group, 'Duplicate view/mode'
        group[run['mode']] = run
    assert all(set(runs) == {'full', 'region', 'control'} for runs in groups.values()), 'Missing view/mode'
    fixed = {name for name, runs in groups.items() if runs['full']['fixture'] == 'fixed-benchmark'}
    assert fixed == set(VIEWS), ('Missing fixed views', fixed)
    if state.get('paint_probe_enabled'):
        assert 'painted-lower-facing' in groups and groups['painted-lower-facing']['full']['fixture'] == 'paint-probe'
    report = {'passed': False, 'benchmark_performed': False, 'samples': 2, 'viewport': [1280, 720],
              'scene_sha256': state['scene_sha256'], 'source_sha256': state.get('source_sha256', {}),
              'engine_integration_manifest_sha256': state.get('engine_integration_manifest_sha256'),
              'results_sha256': hashlib.sha256((folder / 'results.json').read_bytes()).hexdigest(),
              'legacy_results_sha256': hashlib.sha256((legacy / 'results.json').read_bytes()).hexdigest(),
              'legacy_report_sha256': hashlib.sha256((legacy / 'report.json').read_bytes()).hexdigest(),
              'paint_probe_enabled': bool(state.get('paint_probe_enabled', False)),
              'legacy_folder': str(legacy.resolve()), 'cases': [],
              'shading_comparison_policy': 'Corrected full/region/control must match exactly; historical shading changes are quantified.'}
    totals, repairs, conditioned_fixed = Counter(), Counter(), Counter()
    for name, runs in groups.items():
        assert set(runs) == {'full', 'region', 'control'}, (name, 'Missing mode')
        run = runs['full']
        assert all(item['fixture'] == run['fixture'] and item['temporary_overrides'] == run['temporary_overrides']
                   for item in runs.values()), (name, 'Controls changed within a comparison')
        full = {suffix: load(folder, run, suffix) for suffix in FORMATS}
        owned = (full['hit'][..., 3] >> 2) != 0
        assert np.count_nonzero(owned) > run['width'] * run['height'] * 2 * .005, (name, 'Missing tower coverage')
        case = {'view': name, 'fixture': run['fixture'], 'temporary_overrides': run['temporary_overrides'],
                'gradient_context': gradient_summary(full['hit'], full['uv-gradients'], full['depth-gradients']),
                'comparisons': {}, 'legacy': None}
        totals.update(case['gradient_context']['representations'])
        repairs.update(case['gradient_context']['repair_provenance'])
        if run['fixture'] == 'fixed-benchmark':
            assert case['gradient_context']['validity']['valid'] > 0, (name, 'All owned contexts are coarse')
            conditioned_fixed.update(case['gradient_context']['conditioned_representations'])
        for suffix, reference in full.items():
            mask = owned if suffix in ('diffuse', 'specular', 'normal') else np.ones_like(owned)
            if suffix in ('diffuse', 'specular', 'normal'):
                assert np.all(np.isfinite(reference.view('<f2')[owned])), (name, suffix, 'Non-finite shading')
            elif suffix == 'depth':
                assert np.all(np.isfinite(reference.view('<f4')[owned])), (name, 'Non-finite recorded depth')
            checks = {}
            for mode in ('region', 'control'):
                checks[mode] = compare(reference, load(folder, runs[mode], suffix), mask)
                assert checks[mode]['different_samples'] == 0, (name, suffix, mode, checks[mode])
            case['comparisons'][suffix] = checks
        if run['fixture'] == 'fixed-benchmark':
            old_run = old_runs[(name, 'full')]
            baseline = {'hit_depth': {}, 'shading_correction': {}}
            for suffix in ('hit', 'depth', 'diffuse', 'specular', 'normal'):
                previous = load(legacy, old_run, suffix)
                published = old_cases[(2, name)]['comparisons'][suffix]
                assert published['control_different_samples'] == published['region_different_samples'] == 0
                assert sha(previous) == published['full_sha256'], ('Frozen attachment changed', name, suffix)
                if suffix in ('hit', 'depth'):
                    baseline['hit_depth'][suffix] = compare(previous, full[suffix], np.ones_like(owned))
                    if suffix=='hit' and hit_uv_roundoff_ulps:
                        from Float32Roundoff import compare_components
                        baseline['hit_uv_roundoff'] = compare_components(previous,full[suffix],owned,2,hit_uv_roundoff_ulps)
                    else:
                        assert baseline['hit_depth'][suffix]['different_samples'] == 0, (name, suffix, 'Visibility changed')
                else:
                    baseline['shading_correction'][suffix] = shading_delta(previous, full[suffix], owned)
            case['legacy'] = baseline
        elif run['fixture'] == 'paint-probe':
            case['paint_probe'] = validate_paint_probe(folder, run, full, owned, state['paint_probe'], runs)
        report['cases'].append(case)
    report['observed_representation_samples'] = dict(totals)
    report['observed_repair_samples'] = dict(repairs)
    report['conditioned_fixed_view_representation_samples'] = dict(conditioned_fixed)
    assert conditioned_fixed['relief'] + conditioned_fixed['morph'] > 0, 'No conditioned relief or morph samples'
    report['unobserved_representations'] = [name for name in REPRESENTATIONS.values() if not totals[name]]
    report['unobserved_repairs'] = [name for name in REPAIRS.values() if not repairs[name]]
    report['passed'] = True
    return report


class AtlasContractTests(unittest.TestCase):
    def context(self):
        hit = np.zeros((1, 2, 2, 4), dtype='<u4')
        hit[..., 3] = 4
        uv = np.zeros(hit.shape, dtype='<f4')
        uv[..., 0], uv[..., 3] = .01, .02
        depth = np.zeros(hit.shape, dtype='<f4')
        depth[..., 2], depth[..., 3] = 1, 1
        return hit, uv, depth

    def test_sample_matched_metadata_and_repairs(self):
        hit, uv, depth = self.context()
        hit[0, 0, 1, 3] = 8
        depth[0, 0, 1, 2] = 769
        result = gradient_summary(hit, uv.view('<u4'), depth.view('<u4'))
        self.assertEqual(result['mixed_owned_owner_pixels'], 1)
        self.assertEqual(result['repair_provenance']['depth-weighted-bridge'], 1)
        self.assertEqual(result['representations']['relief'], 4)

    def test_invalid_owned_context_is_rejected(self):
        hit, uv, depth = self.context()
        for change in ('nan', 'validity', 'representation', 'coarse-zero', 'uncleared'):
            a, b, c = hit.copy(), uv.copy(), depth.copy()
            if change == 'nan': b[0, 0, 0, 0] = np.nan
            elif change == 'validity': c[0, 0, 0, 3] = 0
            elif change == 'representation': c[0, 0, 0, 2] = 1025
            elif change == 'coarse-zero': c[0, 0, 0, 3] = 2; b[0, 0, 0] = 0
            else: a[0, 0, 0, 3] = 0
            with self.subTest(change=change), self.assertRaises(AssertionError):
                gradient_summary(a, b.view('<u4'), c.view('<u4'))

    def test_dds_four_slot_packing_and_payload(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'atlas.dds'
            header = bytearray(148)
            header[:4], header[84:88] = b'DDS ', b'DX10'
            struct.pack_into('<II', header, 12, 1, 8)
            struct.pack_into('<I', header, 128, 2)
            values = np.arange(32, dtype='<u4').reshape(1, 2, 4, 4)
            path.write_bytes(header + values.tobytes())
            np.testing.assert_array_equal(read_atlas(path, 2, 1, 2, 2, '<u4', 4), values[:, :, :2])
            path.write_bytes(path.read_bytes()[:-1])
            with self.assertRaises(AssertionError): read_atlas(path, 2, 1, 2, 2, '<u4', 4)

    def test_historical_shading_is_quantified(self):
        before = np.ones((1, 1, 2, 4), dtype='<f2')
        after = before.copy(); after[0, 0, 1, 0] = 2
        delta = shading_delta(before.view('<u2'), after.view('<u2'), np.ones((1, 1, 2), dtype=bool))
        self.assertEqual(delta['different_owned_samples'], 1)
        self.assertEqual(delta['rgb_max_abs'], 1)
        self.assertAlmostEqual(delta['rgb_mean_abs'], 1 / 6)

    def test_declared_canonical_generation_rebase(self):
        before = np.zeros((1, 2, 2, 4), dtype='<u4')
        owners = np.array([[[0, 4], [9, 17]]], dtype='<u4')
        before[..., 3] = np.where(owners != 0, (owners << 2) | 1, 0)
        before[..., 0] = np.arange(4, dtype='<u4').reshape(1, 2, 2)
        painted = before.copy()
        owned = owners != 0
        painted[..., 3][owned] = ((owners[owned] + 10) << 2) | 1
        depth = np.full((1, 2, 2, 1), .25, dtype='<f4').view('<u4')
        proof = compare_paint_generation(before, painted, depth, depth, 'canonical-face', 4, 14)
        self.assertEqual(proof['raw_hit_different_samples'], 3)
        self.assertEqual(proof['canonical_owner_remap']['declared_owner_offset'], 10)
        self.assertEqual(proof['hit_after_canonical_owner_remap']['different_samples'], 0)
        for change in ('nonuniform', 'different-pixel', 'wrong-declaration', 'status', 'uv', 'record-z', 'depth', 'background'):
            candidate, candidate_depth, bound = painted.copy(), depth.copy(), 14
            if change == 'nonuniform': candidate[0, 1, 0, 3] += 4
            elif change == 'different-pixel': candidate[0, 1, :, 3] = candidate[0, 1, ::-1, 3]
            elif change == 'wrong-declaration': bound = 15
            elif change == 'status': candidate[0, 0, 1, 3] ^= 1
            elif change == 'uv': candidate[0, 0, 1, 0] ^= 1
            elif change == 'record-z': candidate[0, 0, 1, 2] ^= 1
            elif change == 'depth': candidate_depth[0, 0, 1, 0] ^= 1
            else: candidate[0, 0, 0, 3] = 4
            with self.subTest(change=change), self.assertRaises(AssertionError):
                compare_paint_generation(before, candidate, depth, candidate_depth, 'canonical-face', 4, bound)

    def test_paint_probe_proves_fixed_and_live_contributions(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            def write(stem, suffix, active, fmt):
                height, width, samples, channels = active.shape
                atlas = np.zeros((height, width, 4, channels), dtype=active.dtype)
                atlas[:, :, :samples] = active
                header = bytearray(148)
                header[:4], header[84:88] = b'DDS ', b'DX10'
                struct.pack_into('<II', header, 12, height, width * 4)
                struct.pack_into('<I', header, 128, fmt)
                (folder / f'{stem}-{suffix}.dds').write_bytes(header + atlas.tobytes())
            hit, _, _ = self.context()
            owned = np.ones((1, 2, 2), dtype=bool)
            depth = np.zeros((1, 2, 2, 1), dtype='<u4')
            color = np.full((1, 2, 2, 4), .75, dtype='<f2').view('<u2')
            fixed = np.full((1, 2, 2, 4), .25, dtype='<f2').view('<u2')
            empty = np.zeros_like(color)
            full = {'hit': hit, 'depth': depth, 'diffuse': color, 'normal': empty}
            runs = {mode: {'stem': 'paint-' + mode, 'width': 2, 'height': 1, 'samples': 2}
                    for mode in ('full', 'region', 'control')}
            usage = np.zeros_like(hit); usage[...] = (2, 1, 2, 1)
            for run in runs.values(): write(run['stem'], 'usage', usage, 3)
            for suffix, data, fmt in (('hit', hit, 3), ('depth', depth, 41), ('diffuse', empty, 10), ('normal', empty, 10)):
                write('pristine', suffix, data, fmt)
            write('prefix', 'diffuse', fixed, 10); write('prefix', 'normal', empty, 10)
            status = {'receiver': {'state': 'Receiver UV atlas active', 'ready': True, 'mode': 2, 'hit_face': 1},
                      'fixed_dabs': 2, 'live_dabs': 2, 'render_stats': {'unique_projections': 4, 'batches': 1}}
            probe = {'passed': True, 'cleanup': {'entity_deleted': True, 'receiver_unbound': True},
                     'document': {'Layers': [{'Composite': [{}, {}], 'Strokes': [{'Points': [{}, {}]}]}],
                                  'ReceiverUv': {'Face': 'test-face'}},
                     'face': {'face': 'test-face', 'hit_face': 1}, 'run_status': {mode: status for mode in runs},
                     'pristine_stem': 'pristine', 'prefix_only_stem': 'prefix'}
            checked = validate_paint_probe(folder, runs['full'], full, owned, probe, runs)
            self.assertEqual(checked['atlas_samples'], 4)
            self.assertTrue(checked['consumers_observed']['forward_paint_composite'])
            write('prefix', 'diffuse', color, 10)
            with self.assertRaisesRegex(AssertionError, 'Live tail'):
                validate_paint_probe(folder, runs['full'], full, owned, probe, runs)
            write('prefix', 'diffuse', empty, 10)
            with self.assertRaisesRegex(AssertionError, 'Atlas prefix'):
                validate_paint_probe(folder, runs['full'], full, owned, probe, runs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', nargs='?', type=Path)
    parser.add_argument('output', nargs='?', type=Path)
    parser.add_argument('--legacy', type=Path, default=DEFAULT_LEGACY)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        outcome = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(AtlasContractTests))
        raise SystemExit(0 if outcome.wasSuccessful() else 1)
    assert args.folder is not None, 'Provide a capture folder or --self-test'
    output = validate(args.folder, args.legacy)
    if args.output: args.output.write_text(json.dumps(output, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(output, indent=2))

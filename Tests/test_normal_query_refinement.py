"""Require normal-only AA queries to preserve the previous corrected renderer.

python test_normal_query_refinement.py --baseline <quality-final> \
    --candidate <normal-only-quality> --output <report.json>
python test_normal_query_refinement.py --self-test

The candidate first passes the existing seven-attachment scheduling and legacy
visibility validator. This adds bit-exact full-resolve comparisons with the
pre-refinement, full-surface virtual-normal query captures. No GPU timings are
collected or inferred. Requires numpy.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import unittest

import numpy as np

import test_tower_gradient_context as gradients


POLICY = 'prepared-normal-stencil-with-probe-validity'
VIEWS = (*gradients.VIEWS, 'wide-representations')
COLORS = ('diffuse', 'specular', 'normal')
MODES = {'full', 'region', 'control'}


class RefinementMismatch(AssertionError):
    def __init__(self, report):
        self.report = report
        super().__init__('Normal-query refinement changed captured attachments: ' +
                         ', '.join(report['failures']))


def file_sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def group_runs(state):
    groups, stems = {}, set()
    for run in state['runs']:
        assert run['stem'] not in stems, 'Duplicate capture stem'
        stems.add(run['stem'])
        assert run['view'] in VIEWS, ('Unexpected view', run['view'])
        assert run['mode'] in MODES, ('Unexpected scheduler mode', run['mode'])
        assert run['samples'] == 2 and (run['width'], run['height']) == (1280, 720)
        modes = groups.setdefault(run['view'], {})
        assert run['mode'] not in modes, 'Duplicate view/mode'
        modes[run['mode']] = run
    assert set(groups) == set(VIEWS), ('Missing or extra refinement views', set(groups))
    assert all(set(modes) == MODES for modes in groups.values()), 'Incomplete scheduler controls'
    return groups


def matching_fixture(baseline, candidate):
    assert candidate.get('normal_query_policy') == POLICY, 'Candidate normal-query policy is missing or wrong'
    for label, state in (('baseline', baseline), ('candidate', candidate)):
        assert state['passed'] and state['restored'] and state['saved_level_unchanged'], label
        assert state['temporary_properties_restored'], (label, 'Temporary properties not restored')
        assert state['samples'] == 2 and state['viewport'] == [1280, 720], label
        assert not state.get('paint_probe_enabled', False), (label, 'Requires the five unpainted views')
    for key in ('scene_sha256', 'tower_configuration_sha256', 'effective_tower_configuration_sha256',
                'controls', 'requested_overrides', 'view_poses', 'gradient_contract',
                'engine_integration_manifest_sha256'):
        assert baseline[key] == candidate[key], (key, 'Fixture differs across refinement')
    assert set(baseline['view_poses']) == set(VIEWS), 'Incorrect camera set'
    before, after = group_runs(baseline), group_runs(candidate)
    for view in VIEWS:
        for mode in MODES:
            a, b = before[view][mode], after[view][mode]
            for key in ('fixture', 'temporary_overrides', 'samples', 'width', 'height', 'legacy_view'):
                assert a[key] == b[key], (view, mode, key, 'Run controls differ')
    return before, after


def compare_attachments(before, after):
    assert set(before) == set(gradients.FORMATS) == set(after), 'Missing attachment'
    owned = (before['hit'][..., 3] >> 2) != 0
    assert np.any(owned), 'No owned baseline samples'
    checks = {}
    for suffix in gradients.FORMATS:
        expected_dtype = np.dtype(gradients.FORMATS[suffix][1])
        assert before[suffix].dtype == after[suffix].dtype == expected_dtype, \
            (suffix, 'Expected unsigned attachment bits')
        mask = owned if suffix in COLORS else np.ones_like(owned)
        checks[suffix] = gradients.compare(before[suffix], after[suffix], mask)
    return checks, int(np.count_nonzero(owned))


def require_exact(checks):
    for suffix, check in checks.items():
        assert check['different_samples'] == 0, (suffix, check)


def validate(baseline, candidate):
    baseline, candidate = Path(baseline), Path(candidate)
    before_state = gradients.read_json(baseline / 'results.json')
    after_state = gradients.read_json(candidate / 'results.json')
    before_runs, after_runs = matching_fixture(before_state, after_state)

    published = gradients.read_json(baseline / 'report.json')
    assert published['passed'] and not published['benchmark_performed'], 'Baseline validation did not pass'
    assert published['results_sha256'] == file_sha(baseline / 'results.json'), 'Baseline result metadata changed'
    published_cases = {case['view']: case for case in published['cases']}
    assert set(published_cases) == set(VIEWS), 'Published baseline has a different view set'

    # Re-run current scheduling, conditioning, finite/clear behavior and strict
    # legacy hit/depth checks; their earlier report alone is not a substitute.
    current = gradients.validate(candidate)
    report = {'passed': False, 'benchmark_performed': False, 'timings_collected': False,
              'normal_query_policy': POLICY, 'samples': 2, 'viewport': [1280, 720],
              'comparison_policy': 'All hit/depth/gradient samples and owned shaded samples must be bit-exact.',
              'baseline_folder': str(baseline.resolve()), 'candidate_folder': str(candidate.resolve()),
              'baseline_results_sha256': file_sha(baseline / 'results.json'),
              'baseline_report_sha256': file_sha(baseline / 'report.json'),
              'candidate_results_sha256': file_sha(candidate / 'results.json'),
              'baseline_source_sha256': before_state['source_sha256'],
              'candidate_source_sha256': after_state['source_sha256'],
              'engine_integration_manifest_sha256': after_state['engine_integration_manifest_sha256'],
              'current_scheduling_and_legacy_validation': current,
              'cases': [], 'failures': []}
    for view in VIEWS:
        before = {suffix: gradients.load(baseline, before_runs[view]['full'], suffix)
                  for suffix in gradients.FORMATS}
        after = {suffix: gradients.load(candidate, after_runs[view]['full'], suffix)
                 for suffix in gradients.FORMATS}
        for suffix, reference in before.items():
            published_checks = published_cases[view]['comparisons'][suffix]
            for mode in ('region', 'control'):
                assert published_checks[mode]['different_samples'] == 0, 'Frozen baseline was unstable'
                assert gradients.sha(reference) == published_checks[mode]['reference_sha256'], \
                    ('Baseline attachment changed', view, suffix)
        checks, owned = compare_attachments(before, after)
        report['cases'].append({'view': view, 'fixture': before_runs[view]['full']['fixture'],
                                'temporary_overrides': before_runs[view]['full']['temporary_overrides'],
                                'owned_samples': owned, 'comparisons': checks})
        report['failures'].extend(f'{view}/{suffix}' for suffix, check in checks.items()
                                  if check['different_samples'] != 0)
    report['owned_samples'] = sum(case['owned_samples'] for case in report['cases'])
    report['passed'] = not report['failures']
    if report['failures']:
        raise RefinementMismatch(report)
    return report


class NormalQueryRefinementTests(unittest.TestCase):
    def attachments(self):
        arrays = {name: np.zeros((1, 2, 2, channels), dtype=dtype)
                  for name, (_, dtype, channels) in gradients.FORMATS.items()}
        arrays['hit'][..., 3] = 5  # owner1, ordinary hit
        arrays['hit'][0, 0, 0, 3] = 0
        for name in COLORS:
            arrays[name][...] = np.float16(.25).view(np.uint16)
        return arrays

    def state(self, candidate=False):
        state = {'passed': True, 'restored': True, 'saved_level_unchanged': True,
                 'temporary_properties_restored': True, 'samples': 2, 'viewport': [1280, 720],
                 'paint_probe_enabled': False, 'scene_sha256': 'scene',
                 'tower_configuration_sha256': 'config', 'effective_tower_configuration_sha256': 'normals-on',
                 'controls': {'normals': True}, 'requested_overrides': {'normals': True},
                 'gradient_contract': 'owner-local-first-order-per-raster-pixel',
                 'engine_integration_manifest_sha256': 'engine',
                 'view_poses': {view: {'position': [0, 0, 1], 'rotation': [0, 0, 0]} for view in VIEWS},
                 'runs': []}
        for view in VIEWS:
            for mode in ('full', 'region', 'control'):
                state['runs'].append({'stem': f'2x-{view}-{mode}', 'view': view, 'mode': mode,
                    'samples': 2, 'width': 1280, 'height': 720,
                    'fixture': 'fixed-benchmark' if view in gradients.VIEWS else 'representation-probe',
                    'temporary_overrides': {} if view in gradients.VIEWS else {'5B': False},
                    'legacy_view': view if view in gradients.VIEWS else None})
        if candidate:
            state['normal_query_policy'] = POLICY
        return state

    def test_exact_all_seven_attachments(self):
        before = self.attachments()
        checks, owned = compare_attachments(before, copy.deepcopy(before))
        require_exact(checks)
        self.assertEqual(owned, 3)
        self.assertEqual(set(checks), set(gradients.FORMATS))

    def test_single_bit_owned_corruption_rejected(self):
        before = self.attachments()
        for suffix in gradients.FORMATS:
            after = copy.deepcopy(before)
            after[suffix][0, 1, 1, 0] ^= 1
            with self.subTest(attachment=suffix), self.assertRaises(AssertionError):
                require_exact(compare_attachments(before, after)[0])

    def test_background_color_excluded_but_records_and_gradients_strict(self):
        before = self.attachments()
        for suffix in gradients.FORMATS:
            after = copy.deepcopy(before)
            after[suffix][0, 0, 0, 0] ^= 1
            checks, _ = compare_attachments(before, after)
            if suffix in COLORS:
                require_exact(checks)
            else:
                with self.subTest(attachment=suffix), self.assertRaises(AssertionError):
                    require_exact(checks)

    def test_same_fixture_accepts_new_policy_and_changed_source_hashes(self):
        before, after = self.state(), self.state(candidate=True)
        before['source_sha256'], after['source_sha256'] = {'shader': 'old'}, {'shader': 'new'}
        a, b = matching_fixture(before, after)
        self.assertEqual(set(a), set(b))

    def test_policy_view_controls_and_case_set_mismatches_rejected(self):
        for change in ('missing-policy', 'wrong-policy', 'view', 'control', 'override', 'mode', 'case', 'duplicate'):
            before, after = self.state(), self.state(candidate=True)
            if change == 'missing-policy': after.pop('normal_query_policy')
            elif change == 'wrong-policy': after['normal_query_policy'] = 'full-surface'
            elif change == 'view': after['view_poses']['lower-facing']['position'][0] = 1
            elif change == 'control': after['controls']['normals'] = False
            elif change == 'override': after['runs'][0]['temporary_overrides'] = {'normals': False}
            elif change == 'mode': after['runs'].pop()
            elif change == 'case': after['runs'] = [run for run in after['runs'] if run['view'] != 'exterior']
            else: after['runs'].append(copy.deepcopy(after['runs'][0]))
            with self.subTest(change=change), self.assertRaises(AssertionError):
                matching_fixture(before, after)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--candidate', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        result = unittest.TextTestRunner(verbosity=2).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(NormalQueryRefinementTests))
        return 0 if result.wasSuccessful() else 1
    assert args.baseline and args.candidate and args.output, 'Provide --baseline, --candidate and --output'
    try:
        report = validate(args.baseline, args.candidate)
    except RefinementMismatch as error:
        args.output.write_text(json.dumps(error.report, indent=2) + '\n', encoding='utf-8')
        print(str(error))
        return 1
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"Normal-only refinement passed: {len(report['cases'])} views, {report['owned_samples']:,} owned samples; no timings collected.")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

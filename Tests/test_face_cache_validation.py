"""Verify active face caching against frozen, authenticated tower captures.

python test_face_cache_validation.py --baseline <prepared-normal-quality> \
    --candidate <face-cache-quality> --log <Editor.log> --output <report.json>
No timing is collected or inferred. Requires numpy.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import unittest

import test_normal_query_refinement as normal
import test_tower_gradient_context as gradients
from Float32Roundoff import compare_components
import numpy as np

POLICY = 'generation-coefficients-frame-local-implicit-near-v1'


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def activity(log):
    rows = re.findall(r'FaceCache pass=(\S+) view=(.*?) (?:viewIdentity=(\S+) )?tick=(\d+) batches=(\d+) faces=(\d+)'
                      r'.*?output=ready table=ready', log)
    rows = [dict(pipeline=p, view=v, view_identity=i, tick=int(t), batches=int(b), faces=int(f))
            for p, v, i, t, b, f in rows]
    assert rows and all(r['batches'] > 0 and r['faces'] > 0 for r in rows), 'No successful cache preparation'
    assert len({r['tick'] for r in rows}) >= 5, 'Insufficient fresh-frame preparation evidence'
    return rows


def validate(baseline, candidate, log, uv_roundoff_ulps=0):
    before, after = read(baseline / 'results.json'), read(candidate / 'results.json')
    assert after.get('face_cache_policy') == POLICY and after.get('face_cache_enabled') is True
    assert after.get('validation_scope','full-view-set') == 'full-view-set', 'Focused pilots are not final validation'
    assert after.get('face_cache_validation_sequence') == 'off/on/off'
    old_runs, new_runs = normal.matching_fixture(before, after)
    for runs in new_runs.values():
        for mode, enabled in (('full', False), ('region', True), ('control', False)):
            assert runs[mode].get('face_cache_enabled') is enabled, (mode, 'Incorrect cache toggle')
        assert runs['region'].get('face_cache_trace', {}).get('same_frame_preparation_depth_forward_ticks'), \
            'Missing per-view preparation and both consumer bindings'
    frozen = read(baseline / 'refinement-report.json')
    assert frozen['passed'] and frozen['candidate_results_sha256'] == normal.file_sha(baseline / 'results.json')
    frozen = frozen['current_scheduling_and_legacy_validation']
    assert frozen['passed'] and frozen['results_sha256'] == normal.file_sha(baseline / 'results.json')
    published = {case['view']: case for case in frozen['cases']}
    current = gradients.validate(candidate,hit_uv_roundoff_ulps=uv_roundoff_ulps)
    cases = []
    for view in normal.VIEWS:
        a = {s: gradients.load(baseline, old_runs[view]['full'], s) for s in gradients.FORMATS}
        b = {s: gradients.load(candidate, new_runs[view]['full'], s) for s in gradients.FORMATS}
        for suffix, image in a.items():
            for mode in ('region', 'control'):
                check = published[view]['comparisons'][suffix][mode]
                assert check['different_samples'] == 0 and gradients.sha(image) == check['reference_sha256']
        comparisons, owned = normal.compare_attachments(a, b)
        mask = (a['hit'][...,3] >> 2) != 0
        roundoff = {}
        for suffix,columns in (('hit',2),('uv-gradients',4),('depth-gradients',2)):
            roundoff[suffix] = compare_components(a[suffix],b[suffix],mask,columns,uv_roundoff_ulps)
        normal.require_exact({s:c for s,c in comparisons.items() if s not in roundoff})
        cases.append(dict(view=view, owned_samples=owned, comparisons=comparisons,roundoff=roundoff))
    source = log.read_text(encoding='utf-8', errors='replace')
    records = activity(source)
    return dict(passed=True, benchmark_performed=False, timings_collected=False,
                face_cache_policy=POLICY, samples=2, viewport=[1280, 720],
                cache_sequence='off/on/off',
                uv_gradient_roundoff_limit_ulps=uv_roundoff_ulps,
                exact_fields=['ownership','status','stored depth','depth attachment','representation','validity','all shaded outputs','off/on/off comparisons'],
                baseline_folder=str(baseline.resolve()), candidate_folder=str(candidate.resolve()),
                baseline_results_sha256=normal.file_sha(baseline / 'results.json'),
                candidate_results_sha256=normal.file_sha(candidate / 'results.json'),
                log_sha256=hashlib.sha256(log.read_bytes()).hexdigest(),
                active_preparation=records, owned_samples=sum(c['owned_samples'] for c in cases),
                cases=cases, current_scheduling_and_legacy_validation=current)


class ActivityProofTests(unittest.TestCase):
    def test_roundoff_bound_and_exact_metadata(self):
        a=np.ones((1,1,1,4),dtype='<f4').view('<u4');mask=np.ones((1,1,1),dtype=bool)
        for steps in (1,2):
            b=a.copy();b[...,0]+=steps
            self.assertEqual(compare_components(a,b,mask,2,2)['max_component_ulps'],steps)
        for change in ('three-ulps','depth','owner','sky'):
            b=a.copy();m=mask.copy()
            if change=='three-ulps':b[...,0]+=3
            elif change=='depth':b[...,2]+=1
            elif change=='owner':b[...,3]+=1
            else:m[:]=False;b[...,0]+=1
            with self.subTest(change=change),self.assertRaises(AssertionError):
                compare_components(a,b,m,2,2)
    def test_requires_success_and_multiple_frames(self):
        for text in ('', 'FaceCache output=ready',
                     'FaceCache pass=P view=V tick=1 batches=1 faces=4 output=ready table=ready'):
            with self.assertRaises(AssertionError):
                activity(text)

    def test_rejects_empty_preparation(self):
        text = '\n'.join(f'FaceCache pass=P view=V tick={n} batches=0 faces=0 output=ready table=ready' for n in range(5))
        with self.assertRaises(AssertionError):
            activity(text)

    def test_accepts_successful_fresh_frames(self):
        text = '\n'.join(f'FaceCache pass=P view=V tick={n} batches=3 faces=40 output=ready table=ready' for n in range(5))
        self.assertEqual(len(activity(text)), 5)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline', 'candidate', 'log', 'output'):
        parser.add_argument('--' + name, type=Path)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--uv-roundoff-ulps',type=int,choices=(0,2),default=0)
    args = parser.parse_args()
    if args.self_test:
        outcome = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ActivityProofTests))
        raise SystemExit(0 if outcome.wasSuccessful() else 1)
    assert all((args.baseline, args.candidate, args.log, args.output)), 'Provide all four paths'
    result = validate(args.baseline, args.candidate, args.log,args.uv_roundoff_ulps)
    args.output.write_bytes((json.dumps(result, indent=2) + '\n').encode('utf-8'))
    print(f"Face cache validation passed: {len(result['cases'])} views; {result['owned_samples']:,} owned samples.")

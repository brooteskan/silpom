"""Guard center-owned lighting state and the compiled strict shader boundary.

python test_neural_receiver_isolation.py [--dxc path/to/dxc.exe]

The source call graph follows the production normal-only probe entry points.
With DXC, it also follows preprocessed neural lighting/depth functions, so a
legacy traversal hidden behind a helper fails the strictness check.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / 'Assets/Shaders/SilPOM'
MATERIAL = ROOT / 'Assets/Materials/Types/SilPOMMesh'
parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--dxc', type=Path)
arguments, remaining = parser.parse_known_args()
DXC = arguments.dxc
sys.argv = [sys.argv[0], *remaining]


def without_comments(source):
    return re.sub(r'//[^\n]*|/\*.*?\*/', '', source, flags=re.S)


def function_bodies(source):
    source = without_comments(source)
    functions = {}
    # Anchoring at a declaration line avoids treating an if/call as a helper
    # definition. Shader return types include floatN, Surface and SilPom types.
    signatures = re.finditer(
        r'^\s*(?:void|bool|uint|float\w*|real\w*|Surface|SilPom\w+)\s+'
        r'(\w+)\s*\([^;{}]*\)\s*\{', source, re.M)
    for signature in signatures:
        opening = source.index('{', signature.start())
        depth, end = 1, opening + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        functions.setdefault(signature.group(1), []).append(source[opening:end])
    return functions


def reachable(functions, roots):
    pending, found = list(roots), {}
    while pending:
        name = pending.pop()
        if name in found or name not in functions:
            continue
        found[name] = '\n'.join(functions[name])
        pending.extend(re.findall(r'\b(\w+)\s*\(', found[name]))
    return found


def block(source, opening):
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[opening:end]


class StrictGenerationLifecycle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = without_comments((ROOT / 'Source/Mesh.cpp').read_text())

    def method(self, name):
        start = self.source.index('MeshController::' + name + '(')
        return block(self.source, self.source.index('{', start))

    def test_policy_applies_before_replacement_loading_or_failure(self):
        body = self.method('SetConfiguration')
        policy = body.index('EnforceStrictGenerationPolicy();')
        self.assertLess(body.index('m_configuration = config;'), policy)
        self.assertLess(policy, body.index('ConnectAssets();'))
        failed = self.method('OnAssetError')
        self.assertLess(failed.index('EnforceStrictGenerationPolicy();'), failed.index('Release(m_pending)'))

    def test_subscription_exists_before_validation_or_processor_early_returns(self):
        # A rejected strict request can retain a nontrained generation whose
        # publication previously disconnected these callbacks. The scene-duty
        # state machine is exercised separately by StrictNeuralPolicyTests;
        # this check binds its lifetime to every early-return entry point.
        prepare = self.method('Prepare')
        self.assertLess(prepare.index('UpdateSceneSubscription();'), prepare.index('return '))
        enforcement = self.method('EnforceStrictGenerationPolicy')
        self.assertLess(enforcement.index('UpdateSceneSubscription();'),
                        enforcement.index('if(!m_processor)return;'))
        tick = self.method('OnTick')
        self.assertLess(tick.index('EnforceStrictGenerationPolicy();'), tick.index('Prepare();'))

    def test_nontrained_publication_keeps_required_render_subscription(self):
        # Publication must reconcile all duties instead of treating a zero
        # trained-batch count as permission to disconnect the strict barrier.
        tick = self.method('OnTick')
        published = tick.index('m_active = AZStd::move(m_pending);')
        self.assertIn('UpdateSceneSubscription();', tick[published:])
        self.assertNotIn('SceneNotificationBus::Handler::BusDisconnect()', tick)
        prepare = self.method('Prepare')
        self.assertNotIn('SceneNotificationBus::Handler::BusConnect(', prepare)
        self.assertNotIn('SceneNotificationBus::Handler::BusDisconnect()', prepare)

    def test_subscription_tracks_strict_requests_and_retained_duties(self):
        subscription = self.method('UpdateSceneSubscription')
        self.assertIn('duties.requestedStrict=', subscription)
        for generation in ('active', 'pending'):
            self.assertIn(f'duties.{generation}Strict=m_{generation}->strictRequired.load();', subscription)
            self.assertIn(f'duties.{generation}Restoration=m_{generation}->hiddenForInstancing;', subscription)
            self.assertIn(f'duties.{generation}Trained=m_{generation}->trainedModelBatches!=0;', subscription)
        self.assertIn('duties.NeedsNotifications()', subscription)
        self.assertIn('BusIsConnectedId(scene->GetId())', subscription)

    def test_final_render_barrier_suppresses_already_collected_draws(self):
        # Atom's per-mesh shader-flag path can replace packets without sending
        # the usual callback. RasterPass/native submission does not recheck
        # m_enabled after collection, so end-of-preparation needs draw-argument
        # suppression in addition to normal item flags.
        self.assertIn('EnforceStrictGenerationPolicy(true);', self.method('OnEndPrepareRender'))
        policy = self.method('EnforceStrictGenerationPolicy')
        self.assertRegex(policy, r'if\s*\(finalBarrier\s*\|\|\s*!strict\)')
        self.assertRegex(policy, r'SetInstanceCount\(enable\s*\?\s*1u\s*:\s*0u\)')
        self.assertIn('SetResolveBatchPublished(generation->resolveBatches[batch],enable);', policy)

    def test_motion_exception_is_zeroed_after_packet_count_restoration(self):
        # Certification deliberately skips motion because generated meshes
        # disable it. Restoring a whole packet's count must not revive a motion
        # item that an unnotified packet rebuild already put in a view list.
        policy = self.method('EnforceStrictGenerationPolicy')
        restored = policy.index('SetInstanceCount(')
        zero = policy.index('disabled.m_instanceCount=0;', restored)
        applied = policy.index('SetDrawInstanceArgs(disabled)', zero)
        self.assertRegex(policy[restored:zero], r'GetDrawListTag\(i\)\s*==\s*motionTag')
        self.assertLess(zero, applied)

    def test_inaccessible_instanced_packet_inventory_is_hidden(self):
        # GetDrawPackets/SetDrawItemEnabled cannot reach Atom instance-group
        # packets. The unsupported configuration therefore needs the owning
        # handle's visibility gate, while normal suppression keeps preparation
        # running for shader recovery.
        policy = self.method('EnforceStrictGenerationPolicy')
        guard = re.search(r'if\s*\(strict\s*&&\s*AZ::Render::r_meshInstancingEnabled\)', policy)
        self.assertIsNotNone(guard)
        hidden = block(policy, policy.index('{', guard.end()))
        self.assertIn('generation->strictCertified.store(false);', hidden)
        self.assertIn('generation->hiddenForInstancing=true;', hidden)
        self.assertRegex(hidden, r'SetVisible\([^;]+,\s*false\)')
        self.assertRegex(hidden, r'SetResolveBatchPublished\([^;]+,\s*false\)')
        self.assertIn('continue;', hidden)
        restore = policy.index('if(generation->hiddenForInstancing)')
        self.assertRegex(block(policy, policy.index('{', restore)), r'SetVisible\([^;]+,\s*true\)')

    def test_normal_modes_keep_existing_draw_controls(self):
        policy = self.method('EnforceStrictGenerationPolicy')
        unaffected = re.search(r'if\s*\(!strict\s*&&\s*!wasStrict\)\s*continue;', policy)
        self.assertIsNotNone(unaffected)
        self.assertLess(unaffected.start(), policy.index('SetDrawItemEnabled('))

    def test_stale_supervariant_index_cannot_use_default_name_fallback(self):
        start = self.source.index('bool CertifyStrictPacket(')
        certificate = block(self.source, self.source.index('{', start))
        name = certificate.index('GetSupervariantName(')
        round_trip = re.search(
            r'if\s*\(asset->GetSupervariantIndex\(supervariant\)\s*!=\s*'
            r'active\.m_shader->GetSupervariantIndex\(\)\)\s*return false;', certificate)
        self.assertIsNotNone(round_trip)
        self.assertLess(name, round_trip.start())
        self.assertLess(round_trip.start(), certificate.index('StrictNeuralPolicy::Classify('))


LIGHTING_STATE = (
    r'silpomSharedState|silpomAppearance(?:Features|Context|View|T|B|GradientsCoarse)|'
    r'silpomDirectional(?:Hidden|Axes|Cosines)|silpomAngularInverseWidth'
)
STATE_WRITE = re.compile(
    r'\b(' + LIGHTING_STATE + r')\b(?:\[[^\]]+\])?\s*'
    r'(?:=(?!=)|[+*/-]=|\+\+|--)')


class ReceiverIsolation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.functions = {}
        for folder in (SHADERS, MATERIAL):
            for path in folder.rglob('*.azsli'):
                for name, bodies in function_bodies(path.read_text()).items():
                    cls.functions.setdefault(name, []).extend(bodies)

    def test_normal_only_probes_cannot_publish_lighting_state(self):
        for entry in ('SilPomEvaluateLearnedNormalPrepared', 'SilPomEvaluateProxyNormalPrepared'):
            with self.subTest(entry=entry):
                closure = reachable(self.functions, [entry])
                self.assertIn(entry, closure)
                # Sanity check that feature acquisition and decode were
                # followed, rather than just testing the two wrapper bodies.
                expected_decode = 'SilPomDecodeMaterialNormal' if entry == 'SilPomEvaluateLearnedNormalPrepared' \
                    else 'SilPomDecodeAppearanceNormal'
                self.assertIn(expected_decode, closure)
                for helper, body in closure.items():
                    self.assertIsNone(STATE_WRITE.search(body),
                                      f'{entry} publishes center lighting state through {helper}')

    def test_height_helpers_do_not_seed_center_lighting_state(self):
        for entry in ('SilPomFaceHeight', 'SilPomGatherFaceHeight', 'SilPomFaceNormal'):
            with self.subTest(entry=entry):
                closure = reachable(self.functions, [entry])
                self.assertIn(entry, closure)
                for helper, body in closure.items():
                    self.assertIsNone(STATE_WRITE.search(body), f'{helper} publishes lighting state')
                    self.assertNotRegex(body, r'SilPom(?:InitNeuralPhysicalReceiver|PrepareNeuralLocalShadow)\s*\(')

    def test_full_center_acquisition_still_publishes_selected_models(self):
        for entry, state in (
                ('SilPomEvaluateLearnedSurface', 'silpomSharedState'),
                ('SilPomAcquireComparisonMaterial', 'silpomSharedState'),
                ('SilPomAcquireProxyMaterialInput', 'silpomAppearanceFeatures'),
                ('SilPomAcquireDirectionalEstimator', 'silpomDirectionalHidden')):
            with self.subTest(entry=entry):
                writes = {match.group(1) for body in reachable(self.functions, [entry]).values()
                          for match in STATE_WRITE.finditer(body)}
                self.assertIn(state, writes)

    def test_center_uv_is_restored_before_full_center_evaluation(self):
        body = self.functions['EvaluateSurface_SilPomMesh'][0]
        learned = body.index('SilPomEvaluateLearnedNormalPrepared(')
        proxy = body.index('SilPomEvaluateProxyNormalPrepared(')
        restored = re.search(r'silpomLitUv\s*=\s*centerUv\s*;', body).start()
        center = body.index('SilPomEvaluateSurfaceCore(')
        self.assertLess(max(learned, proxy), restored)
        self.assertLess(restored, center)

    def test_receiver_cache_is_not_static_shader_state(self):
        for folder in (SHADERS, MATERIAL):
            for path in folder.rglob('*.azsli'):
                self.assertNotRegex(without_comments(path.read_text()),
                                    r'\bstatic\s+SilPom\w*Receiver\b', str(path))

    def test_lighting_entry_owns_context_after_invalid_hit_guard(self):
        body = self.functions['ApplyDirectionalLights'][0]
        invalid_exit = body.index('return;')
        declaration = body.index('SilPomLightingReceiver receiver;')
        initialization = body.index('SilPomInitLightingReceiver(receiver);')
        first_light = body.index('for(uint index=0;')
        self.assertLess(invalid_exit, declaration)
        self.assertLess(declaration, initialization)
        self.assertLess(initialization, first_light)
        self.assertNotIn('silpomNormalProbesCoarse', body)

    def test_both_spiral_rays_receive_the_center_context(self):
        body = self.functions['ApplySpiralLight'][0]
        rays = re.findall(r'ApplySpiralLightRay\([^;]+;', body)
        self.assertEqual(len(rays), 2)
        self.assertTrue(all('SILPOM_RECEIVER_ARGUMENT' in ray for ray in rays))


class StrictCompiledBoundary(unittest.TestCase):
    @unittest.skipUnless(DXC, 'Pass --dxc to verify the compiled strict boundary')
    def test_strict_lighting_and_trained_depth_cannot_reach_marchers(self):
        with tempfile.TemporaryDirectory(prefix='silpom-strict-receiver-') as folder:
            output = Path(folder) / 'strict.hlsl'
            subprocess.run([str(DXC), '-P', '-Fi', str(output), '-D',
                            'SILPOM_NEURAL_VISIBILITY_ONLY=1',
                            str(ROOT / 'Tests/VisibilityGpu.hlsl')],
                           check=True, capture_output=True, text=True)
            source = output.read_text()
        functions = function_bodies(source)
        self.assertIn('SilPomNeuralOnlyVisibility', functions)
        self.assertIn('SilPomIntersectFace', functions)
        forbidden = {'SilPomMarchLocalShadow', 'SilPomReliefVisibility',
                     'SilPomLearnedReliefVisibility', 'SilPomSearchFace', 'SilPomIntersect'}
        for entry in ('SilPomNeuralOnlyVisibility', 'SilPomNeuralOnlyVisibilityPrepared', 'SilPomIntersectFace'):
            with self.subTest(entry=entry):
                self.assertIn(entry, functions)
                closure = reachable(functions, [entry])
                self.assertFalse(forbidden & closure.keys(), (entry, forbidden & closure.keys()))
        self.assertNotIn('SilPomMarchLocalShadow', source)
        self.assertNotIn('SilPomReliefVisibility', source)


if __name__ == '__main__':
    unittest.main()

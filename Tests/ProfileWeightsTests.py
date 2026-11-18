"""Profile weights extracted by Tools/silpom_profile.py must be bit-identical to
the compiled-in networks (as printed by silpom_profile_weights_dump), for every
shader model, and survive the weights-file text round trip.

python ProfileWeightsTests.py <silpom_profile_weights_dump executable>
"""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'Tools'))
import silpom_profile as sp

HIT_NAMES = {1: 'WeldedWall', 2: 'WeldedStairs', 3: 'WeldedDoor', 4: 'OpenShaftWall',
             5: 'OpenShaftStairs', 6: 'OpenShaftTrim', 7: 'OpenShaftReveal'}


def main(dump):
    compiled = {}
    for line in subprocess.run([dump], check=True, capture_output=True, text=True).stdout.splitlines():
        name, index, bits = line.split()
        compiled.setdefault(name, []).append(int(bits, 16))
    checked = 0
    for model, prefix in HIT_NAMES.items():
        weights = sp.compiled_weights(model)
        # Round trip through the weights file text, as the builder reads it.
        text = json.loads(sp.dump_weights(weights))
        expected = {f'{prefix}{k}': v for k, v in text['HitModel'].items()}
        open_shaft = sp.MODELS[model][2]
        if open_shaft:
            expected.update({f'L2{open_shaft}{k}': v for k, v in text['LevelTwo'].items()})
            expected.update({f'Visibility{open_shaft}{k}': v for k, v in text['Visibility'].items()})
            expected.update({f'PhysicalVisibility{open_shaft}{k}': v for k, v in text['PhysicalVisibility'].items()})
            expected[f'Visibility{open_shaft}Threshold'] = [text['PhysicalVisibilityThreshold']]
        for name, values in expected.items():
            bits = [sp.f32_bits(struct.unpack('<f', struct.pack('<f', float(v)))[0]) for v in values]
            assert name in compiled, f'{name} missing from the dump'
            assert bits == compiled[name], f'model {model}: {name} differs from the compiled header'
            checked += len(bits)
    assert sp.exact_f32('0.1') == struct.unpack('<f', struct.pack('<I', 0x3dcccccd))[0]
    # A full profile: weights written next to it, then checked against the headers.
    with tempfile.TemporaryDirectory() as folder:
        profile = Path(folder) / 'wall.silpomprofile'
        profile.write_text(json.dumps({'Version': 1, 'Height': 'height.png', 'Width': 8, 'HeightPixels': 8,
                                       'Scale': .1, 'Reference': .05, 'AddressMode': 0, 'Role': 'Wall',
                                       'ShaderModel': 4}))
        assert sp.main(['weights', str(profile)]) == 0
        assert (Path(folder) / 'wall.silpomweights.json').exists()
        assert sp.main(['check', str(profile)]) == 0
    print(f'{checked} weights across 7 shader models are bit-identical')


if __name__ == '__main__':
    main(sys.argv[1])

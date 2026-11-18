"""SilPOM surface profiles: a .silpomprofile names the height image a trained
profile was made for and its relief parameters; its .silpomweights.json holds
the trained networks. The SilPOM profile builder runs this tool when the
weights file is missing.

  python silpom_profile.py weights PROFILE [--output WEIGHTS]
      Write the profile's weights. Until per-asset retraining this extracts
      the networks of the profile's ShaderModel from Tools/CompiledNetworks
      (formerly compiled into the shaders), rounding each literal exactly as
      the C++ and shader compilers do.
  python silpom_profile.py check PROFILE
      Validate the profile and confirm its weights equal Tools/CompiledNetworks.

Profile format (paths are relative to the profile file):
{
  "Version": 1,
  "Height": "wall/height.png", "Width": 6656, "HeightPixels": 6144,
  "Scale": 0.10, "Reference": 0.05, "AddressMode": 0,
  "Role": "Wall" | "Opening" | "Surface",
  "ShaderModel": 4,
  "Weights": "wall.silpomweights.json"        (optional; default <stem>.silpomweights.json)
}
Weights format: named arrays, each flattened row-major in the declaration
order of the shader arrays. Sections: HitModel (W0 16x11, B0 16, W1 16x2,
B1 2), and for open-shaft profiles LevelTwo (W0 24x19, W1 24x5, B0 24, B1 5),
Visibility and PhysicalVisibility (W0 12x10, B0 12, W1 12, B1 1) and
PhysicalVisibilityThreshold.
"""
import argparse
from fractions import Fraction
from decimal import Decimal
import json
from pathlib import Path
import re
import struct
import sys

GEM = Path(__file__).resolve().parents[1]
SHADERS = GEM / 'Tools/CompiledNetworks'  # networks formerly compiled into the shaders
WEIGHTS_VERSION = 1
PROFILE_VERSION = 1
ROLES = ('Surface', 'Wall', 'Opening')

HIT = (('W0', 16 * 11), ('B0', 16), ('W1', 16 * 2), ('B1', 2))
LEVEL_TWO = (('W0', 24 * 19), ('W1', 24 * 5), ('B0', 24), ('B1', 5))
VISIBILITY = (('W0', 12 * 10), ('B0', 12), ('W1', 12), ('B1', 1))

# ShaderModel -> (hit header, hit prefix, open-shaft name or None, role)
MODELS = {
    1: ('NeuralWeldedTowerWeights.azsli', 'WeldedWall', None, 'Wall'),
    2: ('NeuralWeldedTowerWeights.azsli', 'WeldedStairs', None, 'Surface'),
    3: ('NeuralWeldedTowerWeights.azsli', 'WeldedDoor', None, 'Surface'),
    4: ('NeuralOpenShaftTowerWeights.azsli', 'OpenShaftWall', 'Wall', 'Wall'),
    5: ('NeuralOpenShaftTowerWeights.azsli', 'OpenShaftStairs', 'Stairs', 'Surface'),
    6: ('NeuralOpenShaftTowerWeights.azsli', 'OpenShaftTrim', 'Trim', 'Opening'),
    7: ('NeuralOpenShaftTowerWeights.azsli', 'OpenShaftReveal', 'Reveal', 'Opening'),
}
NUMBER = r'[-+]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?f\b'


def f32_bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def f32_from_bits(bits):
    return struct.unpack('<f', struct.pack('<I', bits))[0]


def exact_f32(literal):
    """Round a decimal literal to the nearest float32, ties to even, as C does."""
    exact = Fraction(Decimal(literal))
    approx = struct.unpack('<f', struct.pack('<f', float(exact)))[0]
    bits = f32_bits(approx)
    candidates = {b for b in (bits - 1, bits, bits + 1) if 0 <= b < 1 << 32}
    def key(b):
        v = f32_from_bits(b)
        if v != v or v in (float('inf'), float('-inf')):
            return (Fraction(10) ** 40, 1)
        return (abs(Fraction(v) - exact), b & 1)
    return f32_from_bits(min(candidates, key=key))


def header_array(header, name, count):
    text = (SHADERS / header).read_text()
    match = re.search(r'\b' + re.escape(name) + r'(?:\[[^\]]*\])*\s*=\s*(.*?);', text, re.S)
    if not match:
        raise ValueError(f'Missing {name} in {header}')
    literals = re.findall(NUMBER, match[1])
    if len(literals) != count:
        raise ValueError(f'{header}:{name} has {len(literals)} values, expected {count}')
    return [exact_f32(v[:-1]) for v in literals]


def header_scalar(header, name):
    text = (SHADERS / header).read_text()
    match = re.search(r'\b' + re.escape(name) + r'\s*=\s*(' + NUMBER + r')\s*;', text)
    if not match:
        raise ValueError(f'Missing {name} in {header}')
    return exact_f32(match[1][:-1])


def compiled_weights(model):
    if model not in MODELS:
        raise ValueError(f'ShaderModel {model} has no compiled-in weights (1-7)')
    header, prefix, open_shaft, _ = MODELS[model]
    weights = {'Version': WEIGHTS_VERSION, 'ShaderModel': model,
               'HitModel': {k: header_array(header, prefix + k, n) for k, n in HIT}}
    if open_shaft:
        weights['LevelTwo'] = {k: header_array('NeuralTowerLevelTwoWeights.azsli', 'L2' + open_shaft + k, n)
                               for k, n in LEVEL_TWO}
        weights['Visibility'] = {k: header_array('NeuralVisibilityWeights.azsli', 'Visibility' + open_shaft + k, n)
                                 for k, n in VISIBILITY}
        weights['PhysicalVisibility'] = {k: header_array('NeuralVisibilityPhysicalWeights.azsli',
                                                         'PhysicalVisibility' + open_shaft + k, n)
                                         for k, n in VISIBILITY}
        weights['PhysicalVisibilityThreshold'] = header_scalar('VisibilityClassificationThresholds.azsli',
                                                               f'Visibility{open_shaft}Threshold')
    return weights


def float_text(value):
    """Shortest decimal that round-trips this float32 (parsed via double, still exact)."""
    for digits in range(1, 10):
        text = f'{value:.{digits}g}'
        if f32_bits(struct.unpack('<f', struct.pack('<f', float(text)))[0]) == f32_bits(value):
            return text
    return repr(value)


def dump_weights(weights):
    """JSON with one line per array, values as shortest float32 round-trip text."""
    def section(value):
        return '{' + ', '.join(f'"{k}": [' + ', '.join(float_text(x) for x in v) + ']' for k, v in value.items()) + '}'
    lines = []
    for key, value in weights.items():
        if isinstance(value, dict):
            lines.append(f'  "{key}": ' + section(value))
        elif isinstance(value, float):
            lines.append(f'  "{key}": {float_text(value)}')
        else:
            lines.append(f'  "{key}": {json.dumps(value)}')
    return '{\n' + ',\n'.join(lines) + '\n}\n'


def load_profile(path):
    profile = json.loads(Path(path).read_text())
    required = {'Version': int, 'Height': str, 'Width': int, 'HeightPixels': int, 'Scale': (int, float),
                'Reference': (int, float), 'AddressMode': int, 'Role': str, 'ShaderModel': int}
    for key, kind in required.items():
        if not isinstance(profile.get(key), kind) or isinstance(profile.get(key), bool):
            raise ValueError(f'{path}: "{key}" is missing or has the wrong type')
    if profile['Version'] != PROFILE_VERSION:
        raise ValueError(f'{path}: unsupported Version {profile["Version"]}')
    if profile['Role'] not in ROLES or profile['AddressMode'] not in (0, 1):
        raise ValueError(f'{path}: Role must be one of {ROLES} and AddressMode 0 or 1')
    if not 0 <= profile['Reference'] <= 1 or profile['Width'] < 1 or profile['HeightPixels'] < 1:
        raise ValueError(f'{path}: invalid Reference or image size')
    return profile


def weights_path(profile_path, profile):
    profile_path = Path(profile_path)
    name = profile.get('Weights') or profile_path.name[:-len('.silpomprofile')] + '.silpomweights.json'
    return profile_path.parent / name


def main(argv=None):
    cli = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = cli.add_subparsers(dest='command', required=True)
    write = sub.add_parser('weights')
    write.add_argument('profile', type=Path)
    write.add_argument('--output', type=Path)
    check = sub.add_parser('check')
    check.add_argument('profile', type=Path)
    args = cli.parse_args(argv)
    profile = load_profile(args.profile)
    expected = compiled_weights(profile['ShaderModel'])
    if MODELS[profile['ShaderModel']][3] != profile['Role']:
        raise ValueError(f'{args.profile}: ShaderModel {profile["ShaderModel"]} is a {MODELS[profile["ShaderModel"]][3]} model')
    if args.command == 'weights':
        output = args.output or weights_path(args.profile, profile)
        output.write_text(dump_weights(expected), newline='\n')
        print(f'wrote {output}')
        return 0
    actual = json.loads(weights_path(args.profile, profile).read_text())
    def bits(value):
        if isinstance(value, dict):
            return {k: bits(v) for k, v in value.items()}
        if isinstance(value, list):
            return [f32_bits(struct.unpack('<f', struct.pack('<f', float(x)))[0]) for x in value]
        if isinstance(value, float):
            return f32_bits(struct.unpack('<f', struct.pack('<f', value))[0])
        return value
    if bits(actual) != bits(expected):
        print(f'{args.profile}: weights differ from the compiled-in shader model {profile["ShaderModel"]}')
        return 1
    print(f'{args.profile}: OK (bit-identical to shader model {profile["ShaderModel"]})')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, OSError, json.JSONDecodeError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(2)

"""SilPOM surface profiles: a .silpomprofile names the height image a trained
profile was made for and its relief parameters; its .silpomweights.json holds
the trained networks. The SilPOM profile builder runs this tool when the
weights file is missing.

  python silpom_profile.py weights PROFILE [--output WEIGHTS] [--networks DIR]
      Write the profile's weights: the networks of its ShaderModel, each read
      from the trained networks folder (--networks, else the profile's
      "Networks") if a header there defines it, else from
      Tools/CompiledNetworks (the networks formerly compiled into the shaders).
      With a trained folder, a network it lacks is an error, and lighting
      estimators or a seed cache layout it lacks are left out, unless
      --fallback takes them from the reference.
      SilPOM's trainers write such headers (train_welded_tower.py,
      level_one.py, level_two.py, level_three.py, train_physical_visibility.py,
      train_directional_estimators.py, train_angular_edges.py,
      train_model_zero.py). Literals are rounded exactly as the C++ and shader
      compilers do.
  python silpom_profile.py check PROFILE [--networks DIR]
      Validate the profile and confirm its weights equal those networks.

Profile format (paths are relative to the profile file):
{
  "Version": 1,
  "Height": "wall/height.png", "Width": 6656, "HeightPixels": 6144,
  "Scale": 0.10, "Reference": 0.05, "AddressMode": 0,
  "Role": "Wall" | "Opening" | "Surface",
  "ShaderModel": 4,
  "Weights": "wall.silpomweights.json"        (optional; default <stem>.silpomweights.json)
  "Cache": "seed-cache.dds"                   (model 0 only, optional)
  "Networks": "trained"                       (optional folder of trainer headers)
}
ShaderModel 0 profiles may omit Height: they then apply to each binding's own
height image of Width x HeightPixels with their calibration.

Weights format: named arrays, each flattened row-major in the declaration
order of the shader arrays. Sections: HitModel (W0 16x11, B0 16, W1 16x2,
B1 2), and for open-shaft profiles LevelTwo (W0 24x19, W1 24x5, B0 24, B1 5),
Visibility and PhysicalVisibility (W0 12x10, B0 12, W1 12, B1 1) and
PhysicalVisibilityThreshold. Model 0 has Seed (HeightWeights 8x4,
SlopeWeights 8x2, Bias 8, Output 8, OutputBias 1) and, for a seed cache,
CacheLayout (Tiles [x, y] of 512 texels; Rows: per tile row two words of ten
3-bit expert indices). The wall (model 4) also has Estimators: the lighting
comparison heads DirectionalField32, AngularOccluders4, AngularOccluders8 and
AngularEdges4 (W0 32x20, B0 32, W1 outputs x 32, B1; outputs 128, 28, 56, 32).
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
TRAINED = []  # folders of trainer output headers, searched before SHADERS
# With trained networks, missing ones are errors (optional ones are left out)
# unless FALLBACK takes them from the compiled reference instead.
FALLBACK = [False]


class MissingNetwork(ValueError):
    pass
WEIGHTS_VERSION = 1
PROFILE_VERSION = 1
ROLES = ('Surface', 'Wall', 'Opening')

HIT = (('W0', 16 * 11), ('B0', 16), ('W1', 16 * 2), ('B1', 2))
LEVEL_TWO = (('W0', 24 * 19), ('W1', 24 * 5), ('B0', 24), ('B1', 5))
VISIBILITY = (('W0', 12 * 10), ('B0', 12), ('W1', 12), ('B1', 1))
SEED = (('HeightWeights', 8 * 4), ('SlopeWeights', 8 * 2), ('Bias', 8), ('Output', 8))
CACHE_TILE = 512
# Lighting estimator heads: SilPomLightingEstimator value -> name, header, prefix, outputs.
ESTIMATORS = {2: ('DirectionalField32', 'DirectionalEstimatorWeights.azsli', 'SilPomDirectionalField', 128),
              3: ('AngularOccluders4', 'DirectionalEstimatorWeights.azsli', 'SilPomDirectionalCaps4', 28),
              4: ('AngularOccluders8', 'DirectionalEstimatorWeights.azsli', 'SilPomDirectionalCaps8', 56),
              5: ('AngularEdges4', 'AngularEdgeWeights.azsli', 'SilPomAngularEdges', 32)}
ESTIMATOR_NAMES = {mode: value[0] for mode, value in ESTIMATORS.items()}
ESTIMATOR_ARRAYS = (('W0', 32 * 20), ('B0', 32), ('W1', None), ('B1', None))
# Weights buffer offsets (ProfileNetworks.azsli): the estimators follow the
# training image size at SP_NET_ESTIMATORS.
ESTIMATOR_OFFSET = 1213

# ShaderModel -> (hit header, hit prefix, open-shaft name or None, role)
MODELS = {
    0: ('TowerNoMarchWeights.azsli', 'TowerNoMarch', None, 'Surface'),
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


def network_text(header, name):
    """The header text that defines symbol name: a trained one, else the compiled reference."""
    pattern = re.compile(r'\b' + re.escape(name) + r'(?:\[[^\]]*\])*\s*=')
    for folder in TRAINED:
        for path in sorted(Path(folder).glob('*.azsli')):  # the folder itself, not checkpoints below it
            text = path.read_text()
            if pattern.search(text):
                return text
    if TRAINED and not FALLBACK[0]:
        raise MissingNetwork(f'{name} is not in the trained networks ({", ".join(map(str, TRAINED))})')
    return (SHADERS / header).read_text()


def header_array(header, name, count):
    text = network_text(header, name)
    match = re.search(r'\b' + re.escape(name) + r'(?:\[[^\]]*\])*\s*=\s*(.*?);', text, re.S)
    if not match:
        raise ValueError(f'Missing {name} in {header}')
    literals = re.findall(NUMBER, match[1])
    if len(literals) != count:
        raise ValueError(f'{header}:{name} has {len(literals)} values, expected {count}')
    return [exact_f32(v[:-1]) for v in literals]


def header_scalar(header, name):
    text = network_text(header, name)
    match = re.search(r'\b' + re.escape(name) + r'\s*=\s*(' + NUMBER + r')\s*;', text)
    if not match:
        raise ValueError(f'Missing {name} in {header}')
    return exact_f32(match[1][:-1])


def compiled_weights(model, layout=None):
    if model not in MODELS:
        raise ValueError(f'ShaderModel {model} has no compiled-in weights (0-7)')
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
    if model == 0:
        # Optional visibility networks (no Level 2), when a trained folder has them.
        try:
            weights['Visibility'] = {k: header_array('NeuralVisibilityWeights.azsli', 'VisibilityModelZero' + k, n)
                                     for k, n in VISIBILITY}
            weights['PhysicalVisibility'] = {k: header_array('NeuralVisibilityPhysicalWeights.azsli',
                                                             'PhysicalVisibilityModelZero' + k, n) for k, n in VISIBILITY}
            weights['PhysicalVisibilityThreshold'] = header_scalar('VisibilityClassificationThresholds.azsli',
                                                                   'VisibilityModelZeroThreshold')
        except (MissingNetwork, ValueError):
            for key in ('Visibility', 'PhysicalVisibility', 'PhysicalVisibilityThreshold'):
                weights.pop(key, None)
        seed = {k: header_array('NeuralTowerWeights.azsli', f'TowerNeural{k}', n) for k, n in SEED}
        seed['OutputBias'] = [header_scalar('NeuralTowerWeights.azsli', 'TowerNeuralOutputBias')]
        weights['Seed'] = seed
        if layout is not None:
            weights['CacheLayout'] = layout
    if model == 4:
        estimators = compiled_estimators()
        if estimators:
            weights['Estimators'] = estimators
    return weights


def estimator_sizes(outputs):
    return {'W0': 32 * 20, 'B0': 32, 'W1': outputs * 32, 'B1': outputs}


def compiled_estimators():
    """The wall's lighting estimator heads: trained ones, else the reference ones formerly
    compiled into the shaders. Heads a strict trained folder lacks are left out."""
    heads = {}
    for name, header, prefix, outputs in ESTIMATORS.values():
        try:
            heads[name] = {k: header_array(header, prefix + k, n) for k, n in estimator_sizes(outputs).items()}
        except MissingNetwork:
            pass
    return heads


def pack_estimators(estimators, width, height):
    """The estimator part of a profile weights buffer (zeros before it)."""
    packed = [0.0] * ESTIMATOR_OFFSET + [float(width), float(height)]
    for name, _, _, outputs in ESTIMATORS.values():
        for key, count in estimator_sizes(outputs).items():
            values = estimators[name][key]
            assert len(values) == count, f'{name}.{key}'
            packed += values
    return packed


def compiled_layout():
    """The seed cache layout compiled for the 6656 x 6144 tower atlas."""
    text = network_text('NeuralTowerCacheLayout.azsli', 'TowerNeuralTileRows')
    rows = [int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]+)u', text)]
    return {'Tiles': [13, 12], 'Rows': rows}


def layout_for(profile):
    """The compiled layout, if the profile's height image has its tile grid."""
    try:
        layout = compiled_layout()
    except MissingNetwork:
        return None  # trained without a seed cache
    if [profile['Width'], profile['HeightPixels']] == [t * CACHE_TILE for t in layout['Tiles']]:
        return layout
    return None


def float_text(value):
    """Shortest decimal that round-trips this float32 (parsed via double, still exact)."""
    for digits in range(1, 10):
        text = f'{value:.{digits}g}'
        if f32_bits(struct.unpack('<f', struct.pack('<f', float(text)))[0]) == f32_bits(value):
            return text
    return repr(value)


def dump_weights(weights):
    """JSON with one line per array, values as shortest float32 round-trip text."""
    def text(x):
        return str(x) if isinstance(x, int) else float_text(x)

    def section(value):
        return '{' + ', '.join(f'"{k}": [' + ', '.join(text(x) for x in v) + ']' for k, v in value.items()) + '}'
    lines = []
    for key, value in weights.items():
        if isinstance(value, dict) and value and all(isinstance(v, dict) for v in value.values()):
            lines.append(f'  "{key}": {{\n' + ',\n'.join(f'    "{k}": ' + section(v) for k, v in value.items()) + '\n  }')
        elif isinstance(value, dict):
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
    if profile.get('ShaderModel') == 0 and 'Height' not in profile:
        del required['Height']
    if 'Cache' in profile and (profile.get('ShaderModel') != 0 or not isinstance(profile['Cache'], str)):
        raise ValueError(f'{path}: only ShaderModel 0 profiles have a Cache (a relative path)')
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
    write.add_argument('--networks', type=Path)
    write.add_argument('--fallback', action='store_true', help='Take networks the folder lacks from the reference')
    check = sub.add_parser('check')
    check.add_argument('profile', type=Path)
    check.add_argument('--networks', type=Path)
    check.add_argument('--fallback', action='store_true')
    args = cli.parse_args(argv)
    profile = load_profile(args.profile)
    TRAINED.clear()
    FALLBACK[0] = args.fallback
    if args.networks:
        TRAINED.append(args.networks)
    elif profile.get('Networks'):
        TRAINED.append(Path(args.profile).parent / profile['Networks'])
    for folder in TRAINED:
        if not Path(folder).is_dir():
            raise ValueError(f'trained networks folder not found: {folder}')
    expected = compiled_weights(profile['ShaderModel'], layout_for(profile))
    # Reference networks keep the role they were trained for; trained ones take any role.
    if not TRAINED and MODELS[profile['ShaderModel']][3] != profile['Role']:
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
            return [x if isinstance(x, int) else f32_bits(struct.unpack('<f', struct.pack('<f', float(x)))[0]) for x in value]
        if isinstance(value, float):
            return f32_bits(struct.unpack('<f', struct.pack('<f', value))[0])
        return value
    if bits(actual) != bits(expected):
        print(f'{args.profile}: weights differ from the networks of shader model {profile["ShaderModel"]}')
        return 1
    print(f'{args.profile}: OK (bit-identical to shader model {profile["ShaderModel"]}'
          + (f' with networks from {TRAINED[0]})' if TRAINED else ')'))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, OSError, json.JSONDecodeError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(2)

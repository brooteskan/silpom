"""Run the Blender -> O3DE fixture twice at the same asset source path."""
import argparse
import json
import shutil
import subprocess
from pathlib import Path

from validate_roundtrip import negative_tests, validate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--blender', required=True, type=Path)
    parser.add_argument('--asset-processor', required=True, type=Path)
    parser.add_argument('--project', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    source = args.project.resolve() / 'Assets/SilPOMRoundTrip'
    source.mkdir(parents=True, exist_ok=True)
    output = root / 'build/roundtrip'
    results = []
    for generation in ('initial', 'reordered'):
        generated = output / generation
        generated.mkdir(parents=True, exist_ok=True)
        command = [str(args.blender), '--background', '--factory-startup', '--python-exit-code', '1',
                   '--python', str(root / 'Tools/roundtrip_fixture.py'), '--', '--output', str(generated)]
        if generation == 'reordered':
            command.append('--reorder')
        subprocess.run(command, check=True, timeout=120)
        for name in ('roundtrip.fbx', 'roundtrip.silpom.json'):
            shutil.copy2(generated / name, source / name)
        shutil.copy2(root / 'Tests/Fixtures/RoundTrip/roundtrip.fbx.assetinfo', source)
        with (generated / 'asset-processor.log').open('w', encoding='utf-8') as log:
            subprocess.run([str(args.asset_processor), '--project-path=' + str(args.project.resolve()),
                            '--platforms=pc'], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)
        shutil.copy2(source / 'roundtrip.imported.json', generated)
        sidecar = json.loads((generated / 'roundtrip.silpom.json').read_text())
        imported = json.loads((generated / 'roundtrip.imported.json').read_text())
        fbx = (generated / 'roundtrip.fbx').read_bytes()
        result = validate(sidecar, imported, fbx)
        result['negative_checks'] = negative_tests(sidecar, imported, fbx)
        product = args.project / 'Cache/pc/assets/silpomroundtrip/roundtrip.fbx.azmodel'
        if not product.is_file():
            raise RuntimeError('Missing stock model product: ' + str(product))
        result['generation'] = generation
        results.append(result)
    if results[0]['semantic_sha256'] != results[1]['semantic_sha256']:
        raise RuntimeError('Reordered export changed authored semantics')
    report = json.dumps(results, indent=2) + '\n'
    (output / 'result.json').write_text(report, encoding='utf-8')
    print(report)


if __name__ == '__main__':
    main()

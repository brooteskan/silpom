"""Compile the actual legacy traversal against its frozen eager-load oracle.

Run in an MSVC developer environment:
  python Tests/test_heightfield_fetches.py --output <scratch-directory>

The generated include is extracted from current production source, preserving
its preprocessor choices. No optimized traversal is copied into the fixture.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def run(output, compiler):
    root = Path(__file__).resolve().parents[1]
    source = (root / 'Assets/Shaders/SilPOM/Heightfield.azsli').read_text()
    output.mkdir(parents=True, exist_ok=True)
    (output / 'HeightfieldFetchCurrent.azsli').write_text(function(source, 'SilPomHit SilPomIntersectMode('))
    (output / 'HeightfieldAddressCurrent.azsli').write_text(function(source, 'int SilPomAddress('))
    executable = output / 'HeightfieldFetchTests.exe'
    command = [compiler, '/nologo', '/std:c++17', '/O2', '/EHsc', '/W4', '/fp:precise',
               '/I' + str(output), '/Fo' + str(output / 'HeightfieldFetchTests.obj'),
               '/Fe' + str(executable), str(root / 'Tests/HeightfieldFetchTests.cpp')]
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--compiler', default='cl.exe')
    args = parser.parse_args()
    if args.output:
        run(args.output.resolve(), args.compiler)
    else:
        with tempfile.TemporaryDirectory(prefix='silpom-heightfield-fetches-') as folder:
            run(Path(folder), args.compiler)

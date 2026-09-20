"""Build a self-contained, legacy-format Blender add-on ZIP with the shared exporter."""
import argparse
from pathlib import Path
import zipfile


ROOT = Path(__file__).resolve().parents[1]


def package(output):
    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    sources = {
        '__init__.py': ROOT / 'Tools/blender_addon/__init__.py',
        'export_mesh.py': ROOT / 'Tools/export_mesh.py',
        'LICENSE': ROOT / 'LICENSE',
    }
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, source in sources.items():
            # Fixed metadata makes repeated builds of the same sources identical.
            info = zipfile.ZipInfo('silpom_exporter/' + name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, source.read_bytes())
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/addon/silpom_exporter.zip')
    args = parser.parse_args()
    print(package(args.output))

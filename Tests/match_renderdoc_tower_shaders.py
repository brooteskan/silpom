"""Identify captured programs, including O3DE runtime specialization, without editing assets."""
from pathlib import Path
import argparse
import hashlib
import json
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('profile', type=Path, help='RenderDoc profile.json or shader-inventory.json')
parser.add_argument('cache', type=Path, help='Project Cache/pc directory')
parser.add_argument('--shader-directory', type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
inventory = json.loads(args.profile.read_text(encoding='utf-8-sig'))
shader_directory = args.shader_directory or args.profile.parent / 'shaders'
by_hash = {s['sha256']: key for key, s in inventory['shaders'].items()}
def dxil_hash(data):
    if len(data) < 32 or data[:4] != b'DXBC':
        return None
    count = struct.unpack_from('<I', data, 28)[0]
    if 32 + count * 4 > len(data):
        return None
    for i in range(count):
        at = struct.unpack_from('<I', data, 32 + i * 4)[0]
        if at + 8 <= len(data) and data[at:at+4] == b'DXIL':
            length = struct.unpack_from('<I', data, at + 4)[0]
            return hashlib.sha256(data[at+8:at+8+length]).hexdigest()
    return None
by_dxil = {}
captured_by_size = {}
for key, shader in inventory['shaders'].items():
    raw = (shader_directory / (shader['stem'] + '.dxbc')).read_bytes()
    captured_by_size.setdefault(len(raw), []).append((key, raw))
    digest = dxil_hash(raw)
    if digest:
        by_dxil[digest] = key
def decode_vbr(data, offset):
    value = 0
    for group in range(5):
        for bit in range(7):
            at = offset + group * 8 + bit
            value |= ((data[at // 8] >> (at % 8)) & 1) << (group * 7 + bit)
        at = offset + group * 8 + 7
        if ((data[at // 8] >> (at % 8)) & 1) != int(group < 4):
            return None
    return value >> 1
def specialization_match(original, captured):
    # Reproduce only O3DE ShaderUtils::TamperBits on in-memory copies. Source
    # shader files remain untouched. Exclude the re-signed 16-byte DXBC hash.
    working = bytearray(original)
    patches = []
    for _ in range(128):
        mismatch = next((i for i in range(20, len(working)) if working[i] != captured[i]), None)
        if mismatch is None:
            return patches if patches else None
        found = False
        for offset in range(max(160, mismatch * 8 - 39), min(len(working) * 8 - 39, mismatch * 8 + 8)):
            old = decode_vbr(working, offset)
            if old is None or old & 0xffffffffffffff00 != 0x45678900:
                continue
            new = decode_vbr(captured, offset)
            if new is None:
                continue
            candidate = bytearray(working)
            payload = (new << 1) & 0xffffffff
            for group in range(5):
                for bit in range(8):
                    at = offset + group * 8 + bit
                    value = ((payload >> (group * 7 + bit)) & 1) if bit < 7 else int(group < 4)
                    candidate[at // 8] = (candidate[at // 8] & ~(1 << (at % 8))) | (value << (at % 8))
            if candidate[mismatch] == captured[mismatch]:
                working = candidate
                patches.append({'specialization_id': old & 255, 'value': new, 'bit_offset': offset})
                found = True
                break
        if not found:
            return None
    return None
matches = {key: [] for key in inventory['shaders']}
cache = args.cache
files = list(cache.rglob('*.azshadervariant')) + list(cache.rglob('*.azshader'))
for path in files:
    data = path.read_bytes()
    start = 0
    while True:
        offset = data.find(b'DXBC', start)
        if offset < 0:
            break
        start = offset + 4
        if offset + 32 > len(data):
            continue
        size = struct.unpack_from('<I', data, offset + 24)[0]
        if size < 32 or offset + size > len(data):
            continue
        digest = hashlib.sha256(data[offset:offset+size]).hexdigest()
        if digest in by_hash:
            matches[by_hash[digest]].append({'product': path.relative_to(cache).as_posix(), 'offset': offset, 'size': size, 'match': 'complete-container'})
        else:
            digest = dxil_hash(data[offset:offset+size])
            if digest and digest in by_dxil:
                matches[by_dxil[digest]].append({'product': path.relative_to(cache).as_posix(), 'offset': offset, 'size': size, 'match': 'exact-DXIL-chunk'})
            else:
                for key, captured in captured_by_size.get(size, []):
                    patches = specialization_match(data[offset:offset+size], captured)
                    if patches:
                        matches[key].append({'product': path.relative_to(cache).as_posix(), 'offset': offset, 'size': size,
                            'match': 'exact-after-O3DE-specialization-excluding-resigned-container-hash', 'patches': patches})
result = {'method': 'Complete container or DXIL SHA256; alternatively exact program bytes after reproducing O3DE runtime VBR specialization, excluding only the re-signed container hash. Read-only input assets.',
          'shader_products_scanned': len(files), 'matches': matches}
args.output.write_text(json.dumps(result, indent=2) + '\n')
print('Products scanned:', len(files))
print('Shaders matched:', sum(bool(m) for m in matches.values()), '/', len(matches))
unmatched = [key for key, values in matches.items() if not values]
if unmatched:
    print('Unmatched:', ', '.join(unmatched))

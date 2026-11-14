"""Compare frozen saved-settings 4x images, hit records and sample depths.

Usage: python MsaaCostVisualTests.py original-captures optimized-captures
"""
import json
from pathlib import Path
import sys
from NearestHitVisualTests import capture_hash


def compare(before,after):
    a=json.loads((before/'results.json').read_text());b=json.loads((after/'results.json').read_text())
    assert all(r['passed'] and r['restored'] and r['saved_level_unchanged'] for r in (a,b))
    assert a['saved_tower_controls']==b['saved_tower_controls'],'Saved tower controls changed'
    assert a['level_sha256']==b['level_sha256'],'Saved level changed'
    counts={'rgba':0,'hits':0,'sample_depth':0}
    for original in sorted(list(before.glob('*.png'))+list(before.glob('*.dds'))):
        current=after/original.name;assert current.exists(),current
        if original.suffix=='.png':kind='rgba'
        elif original.name.endswith('Records.dds'):kind='hits'
        else:kind='sample_depth'
        if kind=='sample_depth':
            # Ignore DDS metadata; preserve all four actual float depth samples.
            assert original.read_bytes()[148:]==current.read_bytes()[148:],original.name
        else:
            assert capture_hash(original,kind)==capture_hash(current,kind),original.name
        counts[kind]+=1
    assert counts=={'rgba':30,'hits':10,'sample_depth':10},counts
    print(json.dumps({'passed':True,'exact_captures':counts},indent=2))
    return counts


if __name__=='__main__':compare(Path(sys.argv[1]),Path(sys.argv[2]))

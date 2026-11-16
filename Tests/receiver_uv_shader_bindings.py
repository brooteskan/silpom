"""Audit compiled fullscreen-bake resource dependencies before GPU execution."""
import argparse,json
from pathlib import Path

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('shader_directory',type=Path)
    root=parser.parse_args().shader_directory
    report=[]
    supplied={'Bindless','DrawSrg','PassSrg','SceneMaterialSrg','SceneSrg','ViewSrg'}
    for name in ['silpomuvbake','silpomuvbakelearned','silpomuvbakeshared']:
        path=root/f'silpommeshuv_mainpipeline_{name}_dx12.bindingdep.json'
        data=json.loads(path.read_text())
        required={group for group,fields in data.items() if isinstance(fields,dict) and any(
            isinstance(value,dict) and {'MainVS','MainPS'}.intersection(value.get('dependentFunctions',[]))
            for value in fields.values())}
        assert required<=supplied,(name,'Unbound entry-point resources',sorted(required-supplied))
        layout=json.loads((root/f'silpommeshuv_mainpipeline_{name}_dx12.srg.json').read_text())
        declared={srg['id'] for srg in layout['ShaderResourceGroups']}
        # Pipeline layouts can retain declared groups even when their entry
        # points no longer read them. Bind the receiver's real ObjectSrg too.
        assert declared<=supplied|{'ObjectSrg'},(name,'Unbound declared pipeline groups',sorted(declared-supplied-{'ObjectSrg'}))
        report.append({'shader':name,'entry_point_resource_groups':sorted(required)})
    print(json.dumps({'passed':True,'shaders':report},indent=2))

if __name__=='__main__':main()

"""Compile the production traversal and verify divergent texture binding ABI.

Run with --dxc PATH when DXC is not on PATH. Both SM 6.2 and SM 6.7 are checked.
A temporary copy with the production annotations removed must fail the same
guard, proving this detects the binding defect rather than compiler success.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile


ROOT=Path(__file__).resolve().parents[1]
KNOWN_DXC=Path.home()/'.o3de/3rdParty/packages/DirectXShaderCompilerDxc-1.8.2505.1-o3de-rev3-windows/DirectXShaderCompilerDxc/bin/dxc.exe'


def texture_handle_flags(disassembly,profile):
    # The real production include is compiled against the harness ABI:
    # t0 is the table; t1..t64, space0 are paintImages. Ignore the table and s0.
    if profile=='ps_6_2':
        pattern=r'@dx\.op\.createHandle\(i32 57, i8 0, i32 1, i32 [^,]+, i1 (true|false)\)'
    else:
        pattern=(r'@dx\.op\.createHandleFromBinding\(i32 217, '
            r'%dx\.types\.ResBind \{ i32 1, i32 64, i32 0, i8 0 \}, '
            r'i32 [^,]+, i1 (true|false)\)')
    return re.findall(pattern,disassembly)


def assert_nonuniform(disassembly,profile):
    flags=texture_handle_flags(disassembly,profile)
    assert len(flags)>=2,(profile,'Missing color/normal paint texture handle bindings',flags)
    assert all(flag=='true' for flag in flags),(profile,'Paint texture descriptor selection lost nonuniform ABI',flags)
    return len(flags)


def compile_shader(dxc,source,output,profile,entry='MainPS'):
    subprocess.run([str(dxc),'-T',profile,'-E',entry,str(source),'-Fo',str(output)],check=True,capture_output=True,text=True)
    return subprocess.run([str(dxc),'-dumpbin',str(output)],check=True,capture_output=True,text=True).stdout


def assert_engine_nonuniform(disassembly,minimum):
    """Inspect only the engine bindless Texture2D array, space1000."""
    if '@dx.op.createHandleFromBinding(' in disassembly:
        pattern=(r'@dx\.op\.createHandleFromBinding\(i32 217, '
            r'%dx\.types\.ResBind \{ i32 0, i32 -1, i32 1000, i8 0 \}, '
            r'i32 [^,]+, i1 (true|false)\)')
    else:
        binding=re.search(r';\s*Bindless_m_Texture2D\s+texture\s+\w+\s+2d\s+T(\d+)\s+t0,space1000',disassembly)
        assert binding,'Engine bindless Texture2D array missing from DXIL resource ABI'
        pattern=(r'@dx\.op\.createHandle\(i32 57, i8 0, i32 '+binding.group(1)+
            r', i32 [^,]+, i1 (true|false)\)')
    flags=re.findall(pattern,disassembly);nonuniform=flags.count('true')
    assert nonuniform>=minimum,('Actual engine paint texture handles lost nonuniform ABI',nonuniform,minimum,flags)
    return {'total_texture_handles':len(flags),'nonuniform_texture_handles':nonuniform,
        'minimum_nonuniform_handles':minimum}


def inspect_cached_variant(dxc,source,minimum,temporary):
    data=source.read_bytes();offset=0;reports=[]
    while True:
        begin=data.find(b'DXBC',offset)
        if begin<0:break
        offset=begin+4
        if begin+28>len(data):continue
        size,=struct.unpack_from('<I',data,begin+24)
        if size<32 or size>len(data)-begin:continue
        blob=temporary/'cached-stage.dxil';blob.write_bytes(data[begin:begin+size])
        dump=subprocess.run([str(dxc),'-dumpbin',str(blob)],check=True,capture_output=True,text=True).stdout
        if not re.search(r'!\{!"ps", i32',dump):continue
        reports.append(assert_engine_nonuniform(dump,minimum))
    assert len(reports)==1,('Expected one actual cached pixel shader',source,len(reports))
    return {'source':str(source),'sha256':hashlib.sha256(data).hexdigest(),**reports[0]}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dxc',type=Path,default=os.environ.get('SILPOM_DXC') or shutil.which('dxc') or KNOWN_DXC)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--engine-hlsl',nargs=2,action='append',default=[],metavar=('FILE','MIN_HANDLES'),
        help='Also compile an actual generated engine PixelShader for both profiles and verify its bindless array')
    parser.add_argument('--engine-variant',nargs=2,action='append',default=[],metavar=('FILE','MIN_HANDLES'),
        help='Also extract the actual cached .azshadervariant pixel DXBC and verify its bindless array')
    args=parser.parse_args();assert args.dxc.is_file(),('DXC unavailable; use --dxc',str(args.dxc))
    reports=[];engine_reports=[];cached_reports=[]
    with tempfile.TemporaryDirectory(prefix='silpom-nonuniform-') as temporary:
        root=Path(temporary);tests=root/'Tests';tests.mkdir()
        shader=root/'Assets/Shaders/SilPOM/ReceiverUv';shader.mkdir(parents=True)
        shutil.copyfile(ROOT/'Tests/SurfaceTextureGpu.hlsl',tests/'SurfaceTextureGpu.hlsl')
        shutil.copyfile(ROOT/'Assets/Shaders/SilPOM/ReceiverUv/SurfaceNormal.azsli',shader/'SurfaceNormal.azsli')
        production=(ROOT/'Assets/Shaders/SilPOM/ReceiverUv/SurfaceTexture.azsli').read_text()
        omitted,count=re.subn(r'NonUniformResourceIndex\((mip\.[xy])\)',r'\1',production)
        assert count==2,('Expected exactly the production color and normal descriptor annotations',count)
        nullable,count=re.subn(r'Bindless::GetTexture2D\(NonUniformResourceIndex\((mip\.[xy])\)\)',
            r'GetMaterialTexture(NonUniformResourceIndex(\1))',production)
        assert count==2,('Expected direct production bindless calls',count)
        for profile in ('ps_6_2','ps_6_7'):
            actual=compile_shader(args.dxc,ROOT/'Tests/SurfaceTextureGpu.hlsl',root/(profile+'.dxil'),profile)
            handles=assert_nonuniform(actual,profile)
            (shader/'SurfaceTexture.azsli').write_text(omitted)
            negative=compile_shader(args.dxc,tests/'SurfaceTextureGpu.hlsl',root/(profile+'-omitted.dxil'),profile)
            try:assert_nonuniform(negative,profile)
            except AssertionError:pass
            else:raise AssertionError((profile,'Omission negative control unexpectedly passed'))
            negative_flags=texture_handle_flags(negative,profile)
            assert negative_flags and all(flag=='false' for flag in negative_flags),(profile,negative_flags)
            # A marker before the real engine's signed null-fallback branch
            # also disappears. Require that wrapper regression to fail.
            (shader/'SurfaceTexture.azsli').write_text(nullable)
            fallback=compile_shader(args.dxc,tests/'SurfaceTextureGpu.hlsl',root/(profile+'-nullable.dxil'),profile)
            try:assert_nonuniform(fallback,profile)
            except AssertionError:pass
            else:raise AssertionError((profile,'Nullable material helper negative control unexpectedly passed'))
            fallback_flags=texture_handle_flags(fallback,profile)
            assert fallback_flags and all(flag=='false' for flag in fallback_flags),(profile,fallback_flags)
            reports.append({'profile':profile,'paint_texture_handles':handles,'all_nonuniform':True,
                'annotation_omission_rejected':True,'omitted_handle_flags':negative_flags,
                'nullable_material_helper_rejected':True,'nullable_helper_handle_flags':fallback_flags})
        for source,minimum in args.engine_hlsl:
            minimum=int(minimum);assert minimum>=4
            for profile in ('ps_6_2','ps_6_7'):
                dump=compile_shader(args.dxc,Path(source),root/('engine-'+profile+'.dxil'),profile,'PixelShader')
                engine_reports.append({'source':source,'profile':profile,**assert_engine_nonuniform(dump,minimum)})
        for source,minimum in args.engine_variant:
            minimum=int(minimum);assert minimum>=4
            cached_reports.append(inspect_cached_variant(args.dxc,Path(source),minimum,root))
    report={'production_shader':'Assets/Shaders/SilPOM/ReceiverUv/SurfaceTexture.azsli','passed':True,
        'profiles':reports,'engine_shaders':engine_reports,'cached_variants':cached_reports}
    text=json.dumps(report,indent=2)+'\n'
    if args.output:args.output.write_text(text)
    print(text,end='')


if __name__=='__main__':main()

"""Reject stale or failed MSAA products before launching scene validation."""
import json
import re
from pathlib import Path
import sqlite3
import sys


def check(project):
    root=(project/'Gems/silpom/Assets').resolve();cache=project/'Cache/pc';pending=[]
    db=sqlite3.connect((project/'Cache/assetdb.sqlite').resolve().as_uri()+'?mode=ro',uri=True)
    completed={name.lower():stamp/1000.0 for name,status,errors,stamp in db.execute(
        'SELECT s.SourceName,j.Status,j.ErrorCount,j.LastLogTime FROM Jobs j JOIN Sources s ON s.SourceID=j.SourcePK '
        "WHERE j.Platform='pc'") if status==4 and errors==0}
    def fresh(product,inputs,source,generated):
        if not product.exists() or not inputs.exists():return False
        # Preprocessed line directives enumerate the actual includes for each
        # variant; a depth-only edit must not mark unrelated forward roots stale.
        dependencies={Path(p) for p in re.findall(r'^#line \d+ "([^"]+)"',inputs.read_text(),re.M)}
        stamps=[p.stat().st_mtime for p in dependencies if p.is_relative_to(root) and p.exists()]
        if not stamps:return False
        stamps.append(source.stat().st_mtime)
        latest=max(stamps)
        if product.stat().st_mtime>=latest:return True
        # AP retains timestamps on byte-identical products. A completed shader
        # job newer than every input is also valid freshness evidence; otherwise
        # an unchanged NoMSAA product can look stale forever after an MSAA edit.
        key=generated.relative_to(project/'Cache/Intermediate Assets').as_posix().lower() \
            if generated.is_relative_to(project/'Cache/Intermediate Assets') else generated.relative_to(root).as_posix().lower()
        return completed.get(key,0)>=max(latest,generated.stat().st_mtime)
    for source in (root/'Materials/Pipelines/MainPipeline').glob('SilPOMHit*.shader.template'):
        name=source.name.split('.')[0].lower()
        generated=project/'Cache/Intermediate Assets/materials/types/silpommesh'/f'silpommesh_mainpipeline_{name}.shader'
        def variants(path):return [dict(Name='',**v) if 'Name' not in v else v for v in json.loads(path.read_text()).get('Supervariants',[])]
        same_variants=generated.exists() and variants(generated)==variants(source)
        for suffix in ('','-nomsaa'):
            base=cache/'materials/types/silpommesh'/f'silpommesh_mainpipeline_{name}{suffix}_dx12'
            product=Path(str(base)+'_0.azshadervariant')
            if not same_variants or not fresh(product,Path(str(base)+'.azslin'),source,generated):pending.append(str(product))
    for source in (root/'Shaders/SilPOM/HitReuse').glob('*.shader'):
        has_no_msaa=any(v.get('Name')=='NoMSAA' for v in json.loads(source.read_text()).get('Supervariants',[]))
        base=cache/'shaders/silpom/hitreuse'/(source.stem.lower()+('-nomsaa' if has_no_msaa else '')+'_dx12')
        product=Path(str(base)+'_0.azshadervariant')
        if source.stem=='SilPOMFaceCachePrepare':
            ready=fresh(product,Path(str(base)+'.azslin'),source,source)
        else:
            ready=product.exists() and product.stat().st_mtime>=source.with_suffix('.azsl').stat().st_mtime
        if not ready:pending.append(str(product))
    failures=list(db.execute("SELECT s.SourceName,j.Status,j.ErrorCount FROM Jobs j JOIN Sources s ON s.SourceID=j.SourcePK "
        "WHERE (s.SourceName LIKE 'materials/types/silpommesh/%' OR s.SourceName LIKE 'Shaders/SilPOM/HitReuse/%') "
        "AND (j.Status!=4 OR j.ErrorCount>0)"))
    for suffix in ('','-nomsaa'):
        for name in ('silpomhitforwardsrg','silpomhitdepthsrg'):
            reflection=cache/'shaders/silpom/hitreuse'/f'{name}{suffix}_dx12.srg.json'
            groups=json.loads(reflection.read_text()).get('ShaderResourceGroups',[]) if reflection.exists() else []
            group=next((s for s in groups if s['id']=='PassSrg' and s['bindingSlot']==4),None)
            if group is None:
                pending.append(str(reflection)+' (missing preserved PassSrg)')
                continue
            if not any(c['constantId']=='m_silpomViewport' and c['constantByteSize']==16
                       for c in group.get('inputsForSRGConstants',[])):
                pending.append(str(reflection)+' (missing raster viewport constant)')
            if not any(c['constantId']=='m_silpomFaceCacheBatchCount' and c['constantByteSize']==4
                       for c in group.get('inputsForSRGConstants',[])):
                pending.append(str(reflection)+' (missing face-cache availability constant)')
            buffers={b['id']:b['type'] for b in group.get('inputsForBufferViews',[])}
            for name_of_buffer in ('m_silpomFaceCache','m_silpomFaceCacheTable'):
                if buffers.get(name_of_buffer)!='ByteAddressBuffer':
                    pending.append(str(reflection)+f' (missing face-cache buffer {name_of_buffer})')
            if name=='silpomhitforwardsrg':
                inputs={i['id']:i['type'] for i in group.get('inputsForImageViews',[])}
                image='Texture2D<float4>' if suffix else 'Texture2DMS<float4>'
                for sidecar in ('m_silpomHitUvGradients','m_silpomHitDepthGradients'):
                    if inputs.get(sidecar)!=image:
                        pending.append(str(reflection)+f' (wrong sample layout for {sidecar})')
    result=dict(ready=not pending and not failures,stale_products=pending,failed_jobs=failures)
    print(json.dumps(result,indent=2));return result['ready']


if __name__=='__main__':sys.exit(0 if check(Path(sys.argv[1])) else 1)

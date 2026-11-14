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
        key=generated.relative_to(project/'Cache/Intermediate Assets').as_posix().lower()
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
        product=cache/'shaders/silpom/hitreuse'/(source.stem.lower()+'-nomsaa_dx12_0.azshadervariant')
        stamp=source.with_suffix('.azsl').stat().st_mtime
        if not product.exists() or product.stat().st_mtime<stamp:pending.append(str(product))
    failures=list(db.execute("SELECT s.SourceName,j.Status,j.ErrorCount FROM Jobs j JOIN Sources s ON s.SourceID=j.SourcePK "
        "WHERE (s.SourceName LIKE 'materials/types/silpommesh/%' OR s.SourceName LIKE 'Shaders/SilPOM/HitReuse/%') "
        "AND (j.Status!=4 OR j.ErrorCount>0)"))
    for suffix in ('','-nomsaa'):
        reflection=cache/'shaders/silpom/hitreuse'/f'silpomhitforwardsrg{suffix}_dx12.srg.json'
        if not reflection.exists() or not any(s['id']=='PassSrg' and s['bindingSlot']==4
            for s in json.loads(reflection.read_text()).get('ShaderResourceGroups',[])):
            pending.append(str(reflection)+' (missing preserved PassSrg)')
    result=dict(ready=not pending and not failures,stale_products=pending,failed_jobs=failures)
    print(json.dumps(result,indent=2));return result['ready']


if __name__=='__main__':sys.exit(0 if check(Path(sys.argv[1])) else 1)

"""Install the missing portable tower material sources without overwriting edits."""
from pathlib import Path
import argparse,hashlib,json,sys

ROOT=Path(__file__).resolve().parent
FILES=('Materials/Types/TerrainSkirt/TerrainSkirt.materialtype',
       'Materials/Types/TerrainSkirt/TerrainSkirt.azsli')
REQUIRED=('Gems/terrain-compositor/integrations/o3de/Assets/Materials/Types/TerrainMeshBlend/TerrainMeshBlend.materialtype',
          'Gems/terrain-compositor/integrations/o3de/Assets/ShaderLib/TerrainCompositor/TerrainGroundSampling.azsli',
          'Assets/SilPOMPlanarAB/IrishFieldstoneTowerOpenShaft/welded-doorway/terrain-skirt/TowerTerrainSkirtBlend.material',
          'Assets/VueLiveLink/ground_color.png')
def digest(data):return hashlib.sha256(data.replace(b'\r\n',b'\n')).hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project',required=True,type=Path)
    parser.add_argument('--check-only',action='store_true')
    args=parser.parse_args();project=args.project.resolve()
    missing=[path for path in REQUIRED if not (project/path).is_file()]
    if missing:raise ValueError('Restore these dependencies first: '+', '.join(missing))
    plan=[]
    for relative in FILES:
        source=(ROOT/relative).read_bytes();target=project/relative
        if target.exists():
            if digest(target.read_bytes())!=digest(source):raise ValueError('Preserve and reconcile existing local file: '+str(target))
        else:plan.append((target,source))
    if not args.check_only:
        for target,data in plan:target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
    print(json.dumps({'pending' if args.check_only else 'installed':[str(target.relative_to(project)) for target,_ in plan],
                      'asset_processing_required':True,'cpp_rebuild_required':False},indent=2))
if __name__=='__main__':
    try:main()
    except (ValueError,OSError) as error:print('ERROR: '+str(error),file=sys.stderr);sys.exit(1)

"""Blender background fixture: a folded wall, six tagged triangles and two ordinary.

blender --background --factory-startup --python-exit-code 1 --python
Tests/create_planar_mesh_fixture.py -- --output <new project asset directory>
"""
import argparse, json, math, sys
from pathlib import Path
import bpy
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'Tools'))
from export_mesh import export

args=argparse.ArgumentParser();args.add_argument('--output',type=Path,required=True)
output=args.parse_args(sys.argv[sys.argv.index('--')+1:]).output.resolve()
if output.exists() and any(output.iterdir()):
    raise RuntimeError('Use a new output directory to preserve existing fixture files')
output.mkdir(parents=True,exist_ok=True)
bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
vertices=[(x, .6*max(x,0),z) for z in (-1,0,1) for x in (-1,0,1)]
faces=[(0,1,4,3),(1,2,5,4),(3,4,7,6),(4,5,8,7)]
mesh=bpy.data.meshes.new('FoldedWall');mesh.from_pydata(vertices,[],faces)
obj=bpy.data.objects.new('Planar tagged wall',mesh);bpy.context.collection.objects.link(obj)
obj.select_set(True);bpy.context.view_layer.objects.active=obj
for name in ('Shared red','Blue'):
    material=bpy.data.materials.new(name);mesh.materials.append(material)
uv=mesh.uv_layers.new(name='UVMap')
region=mesh.attributes.new('silpom_region_id','INT','FACE')
profile=mesh.attributes.new('silpom_profile_id','INT','FACE')
for polygon in mesh.polygons:
    polygon.material_index=polygon.index%2
    region.data[polygon.index].value=0 if polygon.index==0 else 1
    profile.data[polygon.index].value=0 if polygon.index==0 else 1
    for loop in polygon.loop_indices:
        x,y,z=vertices[mesh.loops[loop].vertex_index];uv.data[loop].uv=((x+1)*.5,(z+1)*.5)
document=export(output,[obj],initialize=True,basename='planar_faces')
size=64
image=bpy.data.images.new('Planar relief',width=size,height=size,float_buffer=True)
image.colorspace_settings.name='Non-Color'
pixels=[]
for y in range(size):
    for x in range(size):
        # A periodic field crosses the coplanar diagonal and the central fold.
        h=.5+.32*math.sin(2*math.pi*(x+.5)/size*4)*math.cos(2*math.pi*(y+.5)/size*4)
        pixels.extend((h,h,h,1))
image.pixels[:]=pixels;image.filepath_raw=str(output/'height.png');image.file_format='PNG';image.save()
(output/'height.png.assetinfo').write_text('''<ObjectStream version="3">
<Class name="TextureSettings" version="2" type="{980132FF-C450-425D-8AE0-BD96A8486177}">
<Class name="Name" field="Preset" value="LUT_R32F" type="{3D2B920C-9EFD-40D5-AAE0-DF131C3D4931}"/>
<Class name="unsigned int" field="SizeReduceLevel" value="0" type="{43DA906B-7DEF-4CA8-9790-854106D3F983}"/>
<Class name="bool" field="EngineReduce" value="false" type="{A0CA880C-AFE4-43CB-926C-59AC48496112}"/>
<Class name="bool" field="EnableMipmap" value="false" type="{A0CA880C-AFE4-43CB-926C-59AC48496112}"/>
</Class></ObjectStream>''')
bindings={}
for i,material in enumerate(document['materials']):
    color=[.6,.12,.04,1] if material['name']=='Shared red' else [.04,.3,.65,1]
    name=f'material_{i}.material';bindings[material['id']]=name
    (output/name).write_text(json.dumps({'materialType':'@gemroot:Atom_Feature_Common@/Assets/Materials/Types/StandardPBR.materialtype','materialTypeVersion':5,'propertyValues':{'baseColor.color':color,'roughness.factor':.7,'general.doubleSided':True}},indent=2))
(output/'fixture.json').write_text(json.dumps({'materials':bindings,'faces':8,'tagged':6,'ordinary':2,'profile':1},indent=2))
print('PLANAR_FIXTURE '+str(output))

# Tower benchmark material dependencies

The original Diversion benchmark publication included
`TowerTerrainSkirtBlend.material`, but omitted its custom project material type
and shader. These two files are the missing dependencies:

- `Materials/Types/TerrainSkirt/TerrainSkirt.materialtype`
- `Materials/Types/TerrainSkirt/TerrainSkirt.azsli`

Run from this SILPOM checkout, supplying the actual TGProject directory:

```powershell
python Integration/TowerBenchmark/install_materials.py --project D:/TG-assets-gameplay/TGProject
```

The installer checks existing content before copying and refuses to overwrite
different local files. It verifies that the terrain gem's imported
`TerrainMeshBlend.materialtype`, the skirt material instance and its existing
`Assets/VueLiveLink/ground_color.png` texture are present. It preserves the
material instance, all assignments and the benchmark scene.

The custom type imports
`@gemroot:TerrainCompositor@/Assets/Materials/Types/TerrainMeshBlend/TerrainMeshBlend.materialtype`
and the shader includes `TerrainCompositor/TerrainGroundSampling.azsli`. Restore
and register the terrain-compositor gem before processing these sources. The
engine integration bundle uses terrain baseline
`ee6887b4b743047f7dece1b179c0cf30cb281880`.

After installing, let Asset Processor build the TerrainSkirt material type,
generated shader variants and `TowerTerrainSkirtBlend.material`. This repair
requires asset processing, not another C++ or O3DE rebuild. Do not copy processed
`Cache` products between PCs. The SILPOM implementation pin remains `e968424`;
this packaging fix does not require changing the profiling branch's base.

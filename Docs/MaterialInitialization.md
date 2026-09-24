# Material initialization regression (issue #7)

Planar mesh materials now copy matching StandardPBR **asset** property values into
the SilPOM template asset and override `general.doubleSided` before creating the
material instance. This preserves image asset references and initializes authored
PSO-affecting properties with their final values. Per-face buffer/image bindings
and surface parameters are still applied to the instance. Ordinary mesh batches
retain their authored sidedness.

Patch materials similarly snapshot the authored asset with double-sided rendering
enabled before instance creation. Neither path changes Atom's runtime PSO policy.
The template and authored source assets are not modified.

## Reproduction

`Tests/material_initialization.py` uses TG's existing `DefaultLevel`,
`Assets/SilPOMPlanarFaces` and `Assets/SilPOM/StoneWall.material` fixtures. Prepare
six derived materials with explicit single/double-sided settings:

```powershell
python Tests/material_initialization.py --prepare --project D:/TG/TGProject
D:/TG/TGProject/build/windows/bin/profile/AssetProcessorBatch.exe --project-path=D:/TG/TGProject --regset-file=D:/TG/TGProject/Gems/silpom/Tests/planar_dx12.setreg
$env:SILPOM_MATERIAL_OUTPUT = 'SilPOMMaterialInitialization'
D:/TG/TGProject/build/windows/bin/profile/Editor.exe --project-path=D:/TG/TGProject --rhi=dx12 --skipWelcomeScreen --autotest_mode --runpython D:/TG/TGProject/Gems/silpom/Tests/material_initialization.py
```

The script observes PSO warnings through `TraceMessageBus` without suppressing
them. It checks the saved planar fixture, reactivates only that fixture, then
tests isolated meshes and patches with both source sidedness values. Every case
must become ready after creation, a displacement edit and component reactivation.
Mesh cases also edit relief shadow steps. Any runtime PSO warning fails the run.
Front/back and relief-disabled captures support separate visual comparison.
The mesh fixtures inherit distinct colors and roughness and add a base-color
texture, exercising asset-property copying. The patch inherits StoneWall's color,
normal, occlusion and height images.

Results and captures are written under `user/<SILPOM_MATERIAL_OUTPUT>`. The script
exits without saving the level. Generated materials live in
`Assets/SilPOMMaterialInitialization` and can be removed after testing.

## Local validation — 2026-09-24

Windows, TG profile build, DX12, 1280 x 720, MSAA 2x. The same harness ran against
the original editor module and then the rebuilt module.

| Operation | PSO warnings before | PSO warnings after |
| --- | ---: | ---: |
| DefaultLevel load | 4 | 0 |
| Reactivate only `SilPOM_Planar_FBX_Edge_Study` | 4 | 0 |
| Mesh, initially single-sided: create / regenerate / reactivate / shadow edit | 4 per operation | 0 |
| Mesh, initially double-sided: same four operations | 4 per operation | 0 |
| Patch, initially single-sided: create / regenerate / reactivate | 2 per operation | 0 |
| Patch, initially double-sided: same three operations | 0 | 0 |
| Total | 46 | 0 |

Disabling the saved fixture produced no PSO warnings; reactivating it alone
reproduced all four level-load messages. These are repeated functor diagnostics
from the fixture's material creation, not evidence of four separate materials.
The fixture reports three batches, including two displaced material groups.

All 16 readiness stages completed after the fix. Ten before/after render pairs
were compared in the fixture region `(480, 200)-(800, 530)`; every compared RGB
pixel was identical. Front/back views preserve the colored, textured displaced
faces, the ordinary batch's authored backface culling, and the textured patch.
Captures with relief shadows enabled and disabled preserve their baseline
appearance. This is a rendering regression check, not a new shadow correctness
oracle. No shaders or shadow algorithms changed.

`SilPOM.Static`, `SilPOM`, `SilPOM.Editor` and `SilPOM.Builder` compiled. A fresh
standalone build passed all five tests: Core, PlanarFaces, FaceNormals,
BlendedArtifacts and ReliefShadows. Asset Processor built all six test materials
with no failures. DefaultLevel and authored materials were not saved or changed.

Local artifacts: `user/SilPOMIssue7Before/result.json`,
`user/SilPOMIssue7After/result.json`, their PNG captures, and
`user/SilPOMIssue7After/image_comparison.json`. Build and editor logs use the
`build/issue7-` prefix in the Gem. The existing unrelated missing FlyCameraInput
viewport-icon error and the projected-shadow `SkinnedMeshes` pass binding error
during Editor shutdown occur in both runs.

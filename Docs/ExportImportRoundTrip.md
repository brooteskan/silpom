# Export/import round-trip fixture

This is a research fixture, not the production curved asset builder. It exercises
Blender FBX export and the real O3DE SceneAPI importer, with an independently checked
sidecar. The checked-in `.blend`, FBX, sidecar, imported graph dump and result are
under `Tests/Fixtures/RoundTrip`.

## Contract

The bent strip contains four explicitly authored triangles and six logical vertices.
Three faces belong to region 9; one is ordinary geometry. Two materials are used,
including a material shared by selected and ordinary faces. A repeat UV seam and
a hard shading edge deliberately split render corners. Logical vertex IDs recover
three shared edges, including the selected/ordinary boundary, without position welding.

The authoring blend stores integer face, region and vertex attributes plus an object
ID. The exporter captures these before splitting selected and ordinary nodes and
before render-corner expansion. This fixture is pretriangulated: arbitrary modifier
and triangulation identity propagation is not implemented.

Four UV channels carry authored UVs, `(face ID, vertex ID)`, displacement direction
XY, and `(direction Z, sentinel 17)`. Directions are constant +Z and independent of
shading normals. Small fixture IDs are exactly representable in float channels;
this is not a general unbounded-ID encoding. The JSON sidecar holds selection,
material and per-corner reference data. SHA-256 binds it and the importer dump to
the exact FBX generation.

Observed importer conventions are explicitly decoded: V is flipped on every UV
channel; channel names survive on the single-material node but become UV0..UV3 on
the multi-material node. `FBX_SCALE_ALL` preserves the expected position units.
Identity and topology checks are exact (integer transport tolerance 1e-5), positions,
UVs and directions use absolute tolerance 2e-5, and normals use 2e-4 because Blender
custom split-normal storage quantizes them.

## Reproduce

The project must contain this gem at `Gems/silpom` and have its O3DE tools built.
Run from the gem directory with a Python 3 interpreter:

```powershell
& 'C:/Program Files/Blender Foundation/Blender 5.0/5.0/python/bin/python.exe' Tools/run_roundtrip.py --blender 'C:/Program Files/Blender Foundation/Blender 5.0/blender.exe' --asset-processor 'D:/code/o3de-development/build/windows_vs2026/bin/profile/AssetProcessorBatch.exe' --project D:/TG/TGProject
```

The runner overwrites the reserved test assets in `Assets/SilPOMRoundTrip`. It exports
and imports twice at that same source path, reversing selected-face export order
on the second pass. Each pass validates the actual imported graph and requires a
stock `.azmodel` product. Semantic signatures must match; FBX byte equality is not
required. Reports and Asset Processor logs are kept in `build/roundtrip`.

Offline validation of the captured baseline needs neither Blender nor O3DE:

```powershell
python Tools/validate_roundtrip.py Tests/Fixtures/RoundTrip
```

The validator also rejects five in-memory corruptions: stale sidecar, fractional
transport identity, duplicate face identity, wrong material and missing sidecar.
These are decoder negative tests, not five separate engine imports. Always run the
validator: an Asset Processor success alone does not prove the diagnostic callback
succeeded. Its exceptions are recorded in the graph dump and rejected by validation.

## Validation and limits

Locally verified using Blender 5.0.1 and the installed O3DE development build:
initial export/import and reordered reimport both pass, preserving four face IDs,
three selected faces, two materials, all corner attributes and all three shared
edges. Each pass also passes all five negative checks. Asset Processor reports zero
failed assets and zero errors, with four warnings: the shared-material meshes have
different imported UV channel names, so Atom declines to merge them. The decoder
handles these names explicitly; this is not a warning-free run.

The diagnostic model intentionally includes both selected and ordinary geometry.
Production selected-face removal, stripping transport channels, sidecar dependency
tracking, duplicate-object ID assignment, modifier mapping, and varying-direction
field fixtures remain separate work. This fixture does not establish the curved
intersection solver's correctness or rendering performance.

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
tracking, and modifier mapping remain separate work. This fixture does not
establish the curved intersection solver's correctness or rendering performance.

## General authoring exporter (transport version 2)

`Tools/export_mesh.py` exports meshes from an existing `.blend` instead of building
the fixed fixture. Example from the Gem directory:

```powershell
& '<blender.exe>' --background '<source.blend>' --python-exit-code 1 --python Tools/export_mesh.py -- --output build/my-export --initialize-identities
```

Use `--object NAME` (repeatable) to restrict export; otherwise all scene meshes are
included. The input file is never overwritten. Continue authoring in the generated
`authoring.blend`, which preserves allocated IDs. Subsequent exports from that
copy do not need `--initialize-identities`. Use a new output directory when the
input itself is named `authoring.blend` in the previous output directory.

Required authoring data:

- An active UV map, assigned material on every face, and an integer FACE-domain
  `silpom_region_id`; zero denotes ordinary geometry.
- Optional integer FACE-domain `silpom_profile_id`; selected faces default to
  profile 1. These are stable profile references, not cooked displacement assets.
- Optional FLOAT_VECTOR POINT- or CORNER-domain `silpom_direction`. Without it,
  captured corner shading normals define displacement directions. Supply an
  explicit field when a hard shading edge must not split displacement geometry.
- Persistent integer `silpom_vertex_id`/`silpom_face_id` attributes and object/
  material identity properties. Initialization allocates missing IDs and repairs
  duplicates in the saved copy. Without initialization, ambiguous IDs are errors.

Object identity namespaces distinguish duplicated or linked meshes. Face identity
combines the object ID, persistent polygon ID, and a rotation-independent oriented
triangle key. Triangulation and corner-data capture happen before partitioning.
Material identities survive renaming; temporary FBX material names carry a stable
identity-derived token. Dense export-local face/vertex keys must be below `2^24`;
they map to persistent identities in the hash-bound sidecar rather than pretending
that arbitrarily large authoring IDs fit into float UV channels.

The helper bakes positive uniform object transforms and scene units into metre
positions. Nonuniform/reflected/sheared transforms, active modifiers, linked
library meshes, and shape keys are rejected. Bake unsupported topology explicitly;
this milestone does not invent modifier identity propagation.

Run authoring regressions and then the actual two-pass engine import:

```powershell
& '<blender.exe>' --background --factory-startup --python-exit-code 1 --python Tests/blender_authoring_tests.py -- --output build/authoring-tests
& '<python.exe>' Tools/run_roundtrip.py --blender '<blender.exe>' --asset-processor '<AssetProcessorBatch.exe>' --project '<project>' --blend build/authoring-tests/duplicate/authoring.blend
```

The generalized runner owns reserved assets in `Assets/SilPOMAuthoredRoundTrip`
and logs/reports in `build/authored-roundtrip`. It does not edit the source blend
or unrelated scene manifests. Decoder-only tests run with
`python Tests/test_roundtrip_metadata.py`, or through CTest when `SILPOM_PYTHON`
is configured.

Verified with Blender 5.0.1 and the local O3DE development build: initial and
reordered imports preserve two objects, eight triangles, six selected faces,
two materials, six shared edges, two selected/ordinary boundaries, varying
directions, and multiple profiles. Both passes share the same semantic hash and
pass all five decoder corruption checks. Asset Processor reports zero failures,
zero errors, and the same four UV-channel merge warnings as the legacy fixture.
Nine Blender authoring checks cover identity initialization, reordering, material
rename, duplicate identity rejection/repair, varying fields/profiles, nonuniform
scale rejection, modifier rejection, and polygon triangulation. Six decoder unit
tests also cover namespaces, stable-ID duplication, winding, and non-finite data.

## Blender add-on

The [README installation and workflow](../README.md#install-and-run-the-blender-exporter)
describe **Curved SilPOM Exporter**, a self-contained legacy-format Blender add-on
ZIP. `Tools/package_blender_addon.py` packages the UI in
`Tools/blender_addon/__init__.py` with the authoritative `Tools/export_mesh.py` and
license. There is no second copy of the transport implementation to maintain.
The add-on requires Blender 5.0 or newer; validation is on 5.0.1 only.

The SilPOM sidebar creates Integer/Face `silpom_region_id` and
`silpom_profile_id` attributes from selected faces in single- or multi-object
Edit Mode. New unassigned faces default to ordinary geometry, and previously
tagged faces retain implicit profile 1 when the profile attribute is first
created. Both assignment and clearing support Blender's Undo. An attribute with
the wrong type/domain is rejected rather than replaced.

The File > Export operator defaults to selected meshes and initializes/repairs
identities. Disable initialization for strict identity validation. For a chosen
`wall.fbx`, it writes `wall.silpom.json` and `wall.authoring.blend` alongside it;
the CLI retains its original `roundtrip.*` and `authoring.blend` names. Both paths
use transport version 2. The authoring copy contains the whole original scene,
not just the exported selection. Export does not bake textures or assign height
maps; profile IDs remain references for future curved assets.

The exporter restores selection, active object and scene units, removes its
temporary collection/objects/meshes/materials, and rolls back identity allocation
on failure. The UI additionally restores Edit Mode when exporting from it.
Successful allocation is retained in the open scene and the authoring copy;
save the working scene normally or continue from the copy to retain those IDs.
Existing bundle files require explicit overwrite opt-in in the add-on, and the
currently open authoring file cannot be an output. FBX generation is staged so a
failed FBX operation does not replace an existing bundle.

Run the installation/operator regression suite without changing the user's
installed add-ons or saving preferences:

```powershell
$env:BLENDER_USER_RESOURCES = "$PWD/build/addon-tests/blender-user"
& '<blender.exe>' --background --factory-startup --python-exit-code 1 --python Tests/blender_addon_tests.py -- --output build/addon-tests
```

Use a dedicated shell for that environment override. The suite builds the ZIP,
installs it with Blender's real add-on installer into a disposable script path,
enables it, and exercises tagging and export through `bpy.ops`. Tests cover
named bundles, paired FBX hashes, repeat export/identity stability, selection
scope, multi-object tagging, scene restoration, input/overwrite guards, invalid
attributes, empty selections, injected FBX failure, and disable/re-enable.
Reports go to `build/addon-tests/result.json`. The generated
`build/addon-tests/integration/roundtrip.authoring.blend` can be passed to
`Tools/run_roundtrip.py --blend` with the Blender, Asset Processor and project
arguments above for a real two-pass TG import.

Alternatively configure CMake with `-DSILPOM_BLENDER=<blender.exe>` to register
`SilPOM.BlenderAuthoring` and `SilPOM.BlenderAddon`; the latter sets an isolated
Blender user-resource directory automatically. None of these tests promote the
experimental transport to a production curved asset builder or runtime renderer.

Validated locally on 2026-09-19 with Blender 5.0.1: all 13 add-on checks and all
five configured CTest suites (core, curved CPU, metadata, Blender authoring and
Blender add-on) pass. The exact add-on-generated FBX/sidecar pair was also imported
through TG's Asset Processor: four faces, three displaced faces, two materials,
three shared edges and one selected/ordinary boundary pass the independent
decoder and its five corruption checks, with a stock `.azmodel` produced.
Asset Processor reports zero failed assets, zero errors and the same four
UV-channel merge warnings described above. Testing used an isolated add-on
installation, not the user's interactive Blender preferences.

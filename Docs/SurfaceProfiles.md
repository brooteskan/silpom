# Trained surface profiles

A trained displacement profile is project data, not prefab or registry data.
It lives in two source files next to each other:

- `<name>.silpomprofile` (JSON): the height image the networks were trained on
  (path relative to the file), its size in pixels (`Width`, `HeightPixels`),
  `Scale` (metres), `Reference`, `AddressMode`, `Role` (`Wall`, `Opening` or
  `Surface`) and `ShaderModel`.
- `<name>.silpomweights.json`: the trained networks, as named arrays flattened
  in the declaration order of the shader arrays (`HitModel`; for open-shaft
  models also `LevelTwo`, `Visibility`, `PhysicalVisibility` and
  `PhysicalVisibilityThreshold`). A `Weights` entry in the profile can name a
  different file.

The SilPOM profile builder turns them into a `.silpomprofileasset`. If the
weights file is missing it runs `Tools/silpom_profile.py weights <profile>`
(engine Python, or `SILPOM_PYTHON`) and writes it next to the profile, so the
weights become part of the project's sources. It also checks that a PNG height
image has the size the profile was trained on.

At runtime each trained batch gets a GPU buffer with the profile's networks
(`SurfaceProfileAsset::PackNetworks`, layout in
`Assets/Shaders/SilPOM/ProfileNetworks.azsli`: hit model, Level 2, visibility,
physical visibility, threshold; each network W0, B0, W1, B1). The material
receives its bindless index (`surface.profileWeights`), the role
(`surface.profileRole`) and which optional networks exist
(`surface.profileFlags`). Shaders no longer contain trained networks, except
model 0's cached seed. Wall records, backup shells and coverage use the role.

`ShaderModel` remains for the canonical wall's learned bundles and lighting
comparisons (model 4) and the model 4 recovery tuning. The networks once
compiled into the shaders are kept in `Tools/CompiledNetworks` as the CPU
reference: `silpom_profile.py weights` extracts a model's networks from there
exactly, and the CPU tools compare against them. Retraining for a level
replaces that source with SilPOM's trainers.

## Assigning profiles

Profiles are assigned per Blender profile ID, once, where the surface is
authored:

1. In Blender's SilPOM panel, choose the profile ID and press **Set...** under
   *trained profile* to pick its `.silpomprofile` (or pass `--profile ID=PATH`
   to `Tools/export_mesh.py`).
2. Export. Besides the FBX and `<name>.silpom.json`, the exporter writes
   `<name>.silpomprofiles`: `{"<profile id>": "<path>.silpomprofile"}`,
   relative to itself. The geometry sidecar does not change with it; Level 4B/5
   learned bundles hash that file's bytes as training data.
3. In O3DE, add a SilPOM Mesh component and choose the surface. Profiles named
   by the surface are used automatically; their height image, displacement,
   reference, address mode and model fields in the component are ignored.
   *Trained profile override* on a displacement profile replaces the surface's
   choice for that ID in one prefab.

Profile IDs the surface does not name keep the per-prefab height image and
relief settings, for untrained (traversed) relief. A binding with a neural
model 1-7 but no trained profile is reported as an error.

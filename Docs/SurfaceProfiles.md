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

`ShaderModel` is transitional: shaders still select compiled-in networks by
model number (1-3 welded tower, 4-7 open-shaft wall, stairs, trim, reveal).
Until per-asset retraining, `silpom_profile.py` produces the weights by
extracting those networks exactly (`silpom_profile.py check` and the
`SilPOM.ProfileWeights` test confirm bit identity). Reading the networks from
the asset instead of the shader headers is the next stage.

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

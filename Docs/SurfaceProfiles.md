# Trained surface profiles

A trained displacement profile is project data, not prefab or registry data.
It lives in two source files next to each other:

- `<name>.silpomprofile` (JSON): the height image the networks were trained on
  (path relative to the file), its size in pixels (`Width`, `HeightPixels`),
  `Scale` (metres), `Reference`, `AddressMode`, `Role` (`Wall`, `Opening` or
  `Surface`) and `ShaderModel` (0-7). Model 0 profiles may omit `Height` (they
  then apply to each binding's own height image of their size and
  calibration) and may name a seed cache image in `Cache`.
- `<name>.silpomweights.json`: the trained networks, as named arrays flattened
  in the declaration order of the shader arrays: `HitModel`; for open-shaft
  models also `LevelTwo`, `Visibility`, `PhysicalVisibility` and
  `PhysicalVisibilityThreshold`; for model 0 `Seed` and, with a seed cache,
  `CacheLayout`; optionally `Estimators`, the lighting comparison heads
  (`DirectionalField32`, `AngularOccluders4`, `AngularOccluders8`,
  `AngularEdges4`; the canonical wall has all four). A `Weights` entry in the
  profile can name a different file.

The SilPOM profile builder turns them into a `.silpomprofileasset`. If the
weights file is missing it runs `Tools/silpom_profile.py weights <profile>`
(engine Python, or `SILPOM_PYTHON`) and writes it next to the profile, so the
weights become part of the project's sources. It also checks that a PNG height
image has the size the profile was trained on.

`silpom_profile.py weights` takes each network from a folder of trained
networks (`--networks DIR`, or the profile's `Networks` folder), or, for a
profile without one, from `Tools/CompiledNetworks`. A network the folder lacks
is an error and optional ones (estimators, seed cache layout) are left out,
unless `--fallback` takes them from `Tools/CompiledNetworks`. SilPOM's trainers write headers
with those symbol names (`train_welded_tower.py`, `level_one.py --train`,
`level_two.py`, `level_three.py`, `train_physical_visibility.py`,
`train_directional_estimators.py`, `train_angular_edges.py`,
`train_model_zero.py`), so retraining for a level is: run the trainers on the
level's training manifest, then write the profiles' weights from their
outputs.

At runtime each neural batch gets a GPU buffer with its profile's networks
(`SurfaceProfileAsset::PackNetworks`, layout in
`Assets/Shaders/SilPOM/ProfileNetworks.azsli`: hit model, Level 2, visibility,
physical visibility, threshold, model 0's seed and cache layout, then the
estimators if any; each network W0, B0, W1, B1). The material receives its
bindless index (`surface.profileWeights`), the role (`surface.profileRole`)
and which optional networks exist (`surface.profileFlags`). Shaders contain no
trained networks. Wall records, backup shells and coverage use the role; a
directional/angular estimator can only be selected for a profile that has it.

Model 0 is a profile too. A binding with model 0's calibration and no profile
uses the gem's `Assets/SilPOM/Profiles/ModelZero.silpomprofile` (6656 x 6144,
0.02 m, reference 0.05, clamped), with the binding's height image and
optional *Neural hit cache*.

`ShaderModel` remains for the canonical wall's learned bundles and lighting
comparisons (model 4) and the model 4 recovery tuning. The networks once
compiled into the shaders are kept in `Tools/CompiledNetworks` as the CPU
reference: `silpom_profile.py weights` extracts a model's networks from there
exactly, and the CPU tools compare against them.

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

## Profile Inspector (Tools > SilPOM Profiles)

Select an entity with a SilPOM Mesh. The pane lists each profile ID with what
it resolved to: the surface's profile, a *Trained profile override*, the gem's
model 0 default, or none (untrained traversed relief), and whether the profile
is ready, loading, not yet processed or invalid. For a profile it shows the
source file, calibration, height image (or the binding's, for model 0 without
one), trained size, seed cache, the networks it carries, and whether its
weights are the `Tools/CompiledNetworks` reference or come from a trained
networks folder. The component's status line is shown above the list.

**Train profile...** trains networks for the selected profile ID from this
surface's own data: the faces of its `.silpom.json` tagged with that ID, their
height image and calibration (filled in from the binding, or from the current
profile when retraining). Choose the role (Surface, Wall or Opening) and the
networks (a hit network; a hit network with Level 2 and visibility; or model
0's seed and no-march hit networks), Quick or Full training, the profile file
(default `profiles/<surface>-<id>.silpomprofile` next to the surface) and where
to assign it: this entity's *Trained profile override* (an undoable prefab
edit, made once the Asset Processor has built the profile), every use of the
surface in its `.silpomprofiles` file (a Blender export of the surface rewrites
that file from the add-on's assignments), or nowhere. Any role can have either
network set; model 0 is a Surface. A profile's role and networks are its own:
the shader model number only selects the network layout (and, for model 0,
the seed path), and Level 2 and the visibility networks are optional for any
model.

Training runs `Tools/silpom_train.py` in the background with a progress bar,
using the engine's Python (`python/python.cmd`, whose requirements include
NumPy and Pillow) and the training libraries `silpom_welded_teacher`,
`silpom_level_one` and `silpom_visibility`, which the `SilPOM.Editor` target
builds next to the Editor. `SILPOM_PYTHON` and `SILPOM_TOOLS_BIN` override
them. Labels and fitting run on the CPU.

The hit network is trained in rounds (3 for Full, 2 for Quick): after each
round the CPU solver runs on the training rays with that round's network, and
the rays it misses, fills or gets wrong count three times as much in the next
round. Checkpoints are selected, and results measured, with the solver budget
the mesh renders with (its validation iterations, retry cap and tolerance
multiplier). On the tower's trim at its budget, three rounds took a minute and
beat the reference network trained for that atlas: 46.9% of held-out hits
seeded within 1 mm (reference 43.0%), 2160 of 27138 hits left to the solver to
miss (reference 2254). It writes, next to the profile,
`<name>.training/` (the manifest, `networks/*.azsli`, `report.json` and
images) and then the profile, whose `Networks` names that folder, and its
weights. Networks the folder lacks are not taken from the tower references:
a trained wall has no lighting estimators until they are trained for it.

Below the list, the pane shows what the selected profile was trained on: the
whole surface with this profile's faces highlighted, the relief with the
faces' UV layout, where the training rays hit it, the spread of view
directions, and where held-out hits are seeded well or badly by the trained
network and by the reference network of the same role, with a summary.

**Assign profile...** uses an existing profile for the selected ID: it lists
the `.silpomprofile` files next to the surface (role, networks, whether the
weights were trained for this surface or are the reference networks, and when)
and can browse for others, then applies the choice as this entity's override
or for every use of the surface. **Clear override** removes this entity's
override, so the ID uses the surface's profile again. Both are undoable prefab
edits.

`silpom_train.py` can also be run directly; see its `--help`.

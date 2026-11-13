SilPOM Mesh: automatic Editor setting compatibility
==================================================

The last Inspector selection wins. Automatic changes are made to the same
component configuration during change notification, before the controller
prepares a new generation. The Inspector refreshes dependent values together.

Selecting 4A disables 4B, 5A, 5B and experimental lighting comparisons.
Selecting 4B disables 4A and keeps compatible 5A/5B selections.
Selecting 5A disables 4A. Selecting 5B also enables its required 5A parent.
Disabling 5A disables 5B. Disabling 5B leaves 5A available independently.
Selecting a lighting comparison enables 4B and disables 4A. Disabling 4B
returns lighting to Current routing, rather than retaining an invalid head.

Learned material, proxy, comparison and learned directional controls select
neural-only local visibility. Explicitly choosing another shadow method
disables the modes requiring neural-only instead of leaving hidden conflicts.
Asset/model/calibration requirements are still validated; the UI never assigns
different assets, retrains models or changes numeric quality budgets for this.

Legacy height bounds and neural depth seed select one another off. Trained
profiles require neural depth, so their seed and bounds controls are read-only
with seed on and bounds off. Existing profile asset/model validation remains.

Compatible options remain independent: 4B with Level 5, height-derived normals
with either proxy mode, Level 2 prediction, R16 conversion, Gather validation,
and decal settings. Single-choice enums already exclude alternative values.
The Patch component has no additional competing Boolean algorithms.

On activation, older ambiguous Editor configurations are repaired once with
shared/comparison and proxy dependencies taking precedence over 4A. The change
is visible and marked dirty for the next user save; opening a level never saves
it automatically. Valid approved settings are unchanged. Export to Ctrl-G uses
the accepted configuration. Raw externally authored runtime data still goes
through the existing strict validation, rather than silently changing routes.

Diffuse ambient accessibility requires a directional representation (2-5).
Enabling it from Current routing or Physical per-ray selects Angular edges (5)
and its dependencies. Selecting Current/Physical, 4A, disabling 4B or explicitly
leaving neural-only turns accessibility off. Turning accessibility off keeps
the selected directional estimator.

Tests/MeshSettingRulesTests.cpp checks 466944 transitions, dependency closure,
idempotence, both switching directions, and preservation of valid choices.
Tests/MeshSettingRulesEditorSmoke.py exercises actual Editor property callbacks,
repair of existing conflicts and the exported runtime component in Ctrl-G.
It uses TG_SILPOM_ESTIMATOR_OUTPUT for output and never saves the level.

Implementation is Editor-only: Include/SilPOM/MeshSettingRules.h and the
EditorMeshComponent adapter. No shader, neural weights, decal code or runtime
fallback path is changed by these rules.

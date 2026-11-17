# SILPOM asset-agnostic migration: handoff for Claude

## Purpose and scope

Make SILPOM a reusable, independently maintained gem. The gem owns rendering algorithms and validated interfaces. Each consuming project owns its profiles, trained models, source bindings, scene data, and project-specific training/evaluation fixtures.

**Completion criterion:** a second, unrelated asset can be exported, trained, built, and rendered in strict neural-only mode using project-owned metadata and products, with no SILPOM source edits.

This document records a source-analysis plan, not an implementation. The user authorized creating and publishing this handoff only. **No implementation changes, builds, tests, benchmarks, or measurements were performed for this handoff.** Obtain a subsequent implementation instruction before executing the plan. Acceptance checks below are future work, not reported results.

The user's architectural requirement is explicit: a gem with its own repository must not depend on assets in a consuming project's test levels. Removing names alone does not satisfy that requirement.

## Source baseline and important corrections

- Repository: https://github.com/brooteskan/silpom
- Inspected baseline: `ece87563a9fec882e413fdf29feb14ce744edc2b`
- Branch at handoff: `codex/integrate-main-ryzen-5700g-20261116`
- Local checkout: `D:/diversion/TG/TGProject/Gems/silpom`
- Engine source inspected in the earlier lifecycle review: `D:/engines/o3de-tg-profile`
- The checkout was clean before this document was added.
- References below identify files and symbols at this baseline. Reinspect current code before implementing; other work may have advanced it.

### Fixed lifecycle issue: preserve the fix

`MeshController::UpdateSceneSubscription` in `Source/Mesh.cpp` now accounts for requested strict/trained rendering and active/pending generation duties. `EnforceStrictGenerationPolicy` invokes it before the processor early-out. `StrictNeuralPolicy::SceneDuties` represents those duties.

The previously reported gap, where a rejected transition to strict mode could leave render-preparation callbacks disconnected, is fixed in this baseline. Do not reintroduce it or treat it as outstanding.

### Outstanding strict-mode issue: ordered crossing recovery

Inside the `SILPOM_NEURAL_NO_MARCH` branch, `SilPomIntersectFace` calls `SilPomRecoverNearestHit`:

- `Assets/Shaders/SilPOM/BlendedFace.azsli`: call around line 634.
- `Assets/Shaders/SilPOM/NearestHit.azsli`: model-4/grazing-ray gate, travel-dependent count capped at 128 intervals, ordered height samples, adjacent residual crossing detection, then bounded Newton validation.

This is a ray search despite its bounded budget. Batching probes does not change its semantics. Earlier review incorrectly treated exclusion of the generic marcher as sufficient evidence of strict no-march behavior.

**Exclude this routine from strict shaders.** Replace its opportunity with fixed neural multi-hypothesis prediction plus bounded local validation, or an explicitly supported analytic repair representation. Do not move a recovery-enable flag into metadata and continue invoking the search in strict mode.

Removing this recovery can affect grazing coverage. Metadata extraction alone cannot preserve that behavior. Treat the strict correction as a distinct behavior change and do not claim visual equivalence without later authorized correctness validation.

## How the coupling arose in the code

Training compatibility is enforced through fixed project asset paths, numeric model IDs, source dimensions, calibration tables, and one hardcoded latent-producer generation. Numeric IDs also select repair, coverage, and LOD behavior. The builder reads named project files, and the controller loads globally named learned bundles.

Those choices make one training fixture part of the gem's runtime contract. This describes the mechanism visible in the source, not an established account of who chose it or why. Compatibility validation is necessary; its project-specific values belong in assets.

## Repository ownership boundary

| Keep in the gem | Move to / author in the consuming project |
|---|---|
| Generic geometry, intersection, shading, repair, LOD, and conservative bounds | Meshes, materials, textures, levels, and profile assignments |
| Finite supported neural kernel families and versioned interfaces | All trained tensors, thresholds, and content calibration |
| Generic schemas, builders, loaders, validation, and strict enforcement | Profile/model manifests and source references |
| Generic cache formats and resource management | Content-specific cache payloads, tile maps, and layout values |
| Parameterized training/export utilities | Project recipes, datasets, fixture generation, and scene-specific regression data |
| Self-contained synthetic unit fixtures | Integration scenes, golden images, and evaluation reports for particular project assets |

A fixed network architecture is an implementation interface. Its weights and training-domain values are assets. Do not implement an arbitrary graph interpreter merely to remove content coupling.

No permanent legacy content lookup table, default wall bundle, or content-specific shader-generation step should remain in the gem. Project-specific conversion logic belongs outside it.

## Affected-file inventory

Paths in this table are repository-relative for portability. The file groups are an implementation map, not authorization to edit them during this handoff.

| File or group | Existing coupling / responsibility | Planned change |
|---|---|---|
| `Include/SilPOM/Mesh.h` | `MeshProfileBinding::m_neuralModel`; `IsTrained()` checks a nonzero number; controller-wide quality/experimental switches | Explicit profile/model references and resolved capabilities; local mesh profile IDs remain opaque |
| `Source/Mesh.cpp` | Fixed learned-product paths in `ConnectAssets`; exact source-asset identity checks; trained profile table; model-specific eligibility; generation publication | Per-profile/material dependency binding, metadata validation, immutable generations, explicit policies |
| `Include/SilPOM/StrictNeuralPolicy.h` | Shader-product certification and scene duties | Preserve lifecycle protections; extend certification to resolved model/profile contracts and audited strict kernels |
| `Include/SilPOM/LearnedMaterialAsset.h`, `Source/LearnedMaterialAsset.cpp` | Opaque learned payload, magic dispatch, generation identity | Versioned model product with typed validated sections and complete dependency identity |
| `Source/Builder/LearnedMaterialBuilder.h`, `Source/Builder/LearnedMaterialBuilder.cpp` | Fixed project source dependencies and raw concatenated source hashing | Manifest-driven dependencies, canonical compatibility fingerprint, runtime product dependencies |
| `Include/SilPOM/MaterialCodec.h`, `AppearanceCodec.h`, `ExperimentalCodecs.h` | Fixed source/feature dimensions and mip counts | Validate supported shapes, formats, limits, section sizes, and mip chains from metadata |
| `Include/SilPOM/DirectionalEstimatorContract.h` | Hardcoded expected shared-feature generation | Expected producer identity moves into the dependent head bundle |
| `Include/SilPOM/MeshSurfaceAsset.h` | Generic geometry and local material/profile references | Preserve generic geometry boundary; do not assign global semantics to profile IDs |
| `Assets/Shaders/SilPOM/BlendedFace.azsli` | Embedded trained weights; model-ID routing; overloaded mode constants; strict recovery call | Generic predictor dispatch, explicit policies, data-backed tensors, strict recovery correction |
| `Assets/Shaders/SilPOM/NearestHit.azsli` | Ordered ray crossing search | Exclude from strict executable paths; any retained reference path remains explicitly non-strict |
| `Assets/Shaders/SilPOM/NeuralTower.azsli`, `NeuralTowerCacheLayout.azsli` | Content-specific legacy cache lookup and tile layout | Explicit optional bounded cache format with asset-owned layout, or deliberately retire the route |
| `Assets/Shaders/SilPOM/LevelTwo.azsli` | Fixed trained model selection and fallback assumptions | Supported multi-hypothesis kernel with explicit bundle binding and bounded candidate policy |
| Trained `*Weights.azsli` files, embedded arrays, `VisibilityClassificationThresholds.azsli` | Depth, visibility, directional, and angular trained constants | Extract all tensors/thresholds into project model products, including generically named files |
| `NeuralVisibility.azsli`, `VisibilityInputs.azsli` | Numeric model dispatch and physical descriptor network selection | Validated kernel/resource selection; retain fixed descriptor acquisition and same-estimator cache fallback |
| `ExperimentalDecode.azsli`, `AppearanceDecode.azsli`, `SharedVisibility.azsli` | Decoder interfaces and receiver-prefix factoring | Preserve bounded interfaces and accumulation order; bind correct bundle per invocation |
| `LearnedNormalFootprint.azsli`, `LearnedMaterialSurface.azsli`, `ProxyAppearance.azsli`, `ProxyDepth.azsli`, `DirectionalEstimator.azsli` | Fixed atlas dimensions, feature resolutions/mip mappings, normalization and reach conventions | Consume one consistent resolved source/feature contract |
| `Assets/Shaders/SilPOM/Heightfield.azsli` | Content-sized NPOT addressing specializations | Keep exact generic addressing; remove fixture-dimension cases |
| `Assets/Shaders/SilPOM/Quality.azsli` | Fixed reference-resolution convention | Explicit quality policy; do not silently change behavior while externalizing data |
| `Assets/Shaders/SilPOM/ImplicitProxyCoverage.azsli` | Model numbers and sentinel modes control coverage | Explicit validated geometry/coverage capabilities shared by CPU and GPU |
| `Assets/Shaders/SilPOM/HitReuse/SilPOMFaceCachePrepare.azsl` | Sentinel-mode cache preparation | Explicit capability and profile/policy generation |
| `Assets/Materials/Types/SilPOMMesh/SilPOMMesh.azsli` | Model-linked LOD, proxy/repair interpretation, center/probe lifecycle | Descriptor-based policy and footprint inputs; preserve center-state ownership and gradients |
| `Assets/Materials/Types/SilPOMMesh/SilPOMMesh_DirectionalLighting.azsli` | One center receiver spanning directional and spiral queries | Preserve receiver lifetime and backend-specific initialization; use the invocation's model binding |
| `Assets/Materials/Types/SilPOMMesh/*.materialtype`, `SilPOMMesh_State.lua` | Numeric selectors, experimental names, sentinel routing | Explicit model/profile references and derived internal shader properties |
| `Assets/Materials/Pipelines/MainPipeline/SilPOMHitDepth*.azsli` and shader descriptors | Content-specific near-wall/MSAA specialization, model-dependent backup and record flags | Generic supported families and representation policies, with consistent MSAA/NoMSAA semantics |
| `Assets/Materials/Pipelines/MainPipeline/SilPOMHitShaderEnable.lua`, pipeline material files including LowEndPipeline | Content-named internal LOD routing | Generic capability-driven enablement; unsupported pipelines remain explicit failures in strict mode |
| `Source/ResolvePass.cpp` | `GetWallCapability`, model IDs 1/4, special knowledge of forced MSAA model 4, sentinel cache eligibility | Certified repair producer/profile capability and complete cache identities |
| `Source/RepairPass.cpp` | Paired regional/full-screen execution and MSAA scratch handling | Preserve pairing and initialization while replacing content-specific producer semantics |
| `Source/Editor/EditorModule.cpp` | Content-named model options and tooltips | Profile/model asset selection, capabilities, actionable compatibility diagnostics |
| `Tools/train_*.py`, `Tools/frozen_bundle.py`, related teachers/exporters | Content-specific paths, dimensions, model lists and output-to-source workflows | Keep only parameterized reusable tools in gem; move recipes and content mappings to project |
| `Assets/Materials/Tests/AngularEdgesIndirect.material` | Inherits a material in the consuming project's test content | Move the integration fixture outside the gem |
| `Tests/`, `Docs/`, build/install manifests | Mixed generic and content-specific references/fixtures | Retain portable synthetic coverage; move project-specific integration data and update packaging |

Audit the entire tree, not just files named after the tower. This handoff is documentation of historical coupling; it is not a new runtime dependency.

## Target data design

### Surface profile asset: authored binding

A profile describes:

- Height image reference, channel, encoding, legal height range, and interpretation.
- Displacement scale/reference, units, direction convention, addressing, and UV transform.
- References to model bundles required by the selected representations.
- Optional learned material, proxy appearance, and proxy depth bindings.
- Explicit intersection, coverage, LOD, repair, and continuity policies.
- Permitted authored overrides.

Profile IDs on mesh faces remain local references. They must not select network weights or imply a geometric class.

### Model bundle: trained resources and compatibility

A bundle describes:

- Container version; supported kernel family and feature-interface version.
- Typed tensor sections, formats, shapes, offsets, lengths, and resource identities.
- Learned field dimensions, channels, mip counts, decode scale/bias, and sampling contract.
- Input normalization, coordinate conventions, source-to-feature mapping, and output interpretation.
- Expected profile calibration or an explicitly supported parameter domain.
- Classification thresholds and bounded validation/candidate limits.
- Source fingerprints for the height/material/geometry actually used during training.
- Latent producer identity and the exact producer/interface required by each dependent head.
- Optional cache layout and supported analytic representation interfaces.
- Reproducible training/export provenance.

The profile supplies actual parameters; the bundle declares compatible parameters. Begin with exact compatibility matching to preserve current calibration. Do not broaden a model's supported domain during data extraction.

Optional heads are genuinely optional. A bundle need not contain every estimator, but a selected rendering policy must require all of its dependencies.

### Kernel ABI stays under gem control

Use a finite registry of supported implementations, initially covering existing families:

- Single-hypothesis depth predictor.
- Multi-hypothesis depth predictor.
- Physical neural visibility.
- Learned material and shared visibility.
- Joint proxy surface/light response and proxy depth.
- Directional-field and angular estimators.

Each interface fixes feature ordering, tensor shapes, activation semantics, output meaning, numeric format, and bounded execution rules. Assets provide data, not executable shader code or arbitrary loop programs.

Keep fixed unrolled evaluators where appropriate. Resolve family selection at batch/material preparation; do not dispatch from semantic content IDs per pixel.

New content requires compatible trained data. Asset-agnostic operation does not make existing weights valid for unrelated surfaces.

## Builder, fingerprint, and format changes

1. Consume a supplied manifest and its declared references. Never construct named test-project paths.
2. Register source dependencies and required build-job dependencies in asset processing.
3. Validate tensor/resource structure and the profile/model/source compatibility contract.
4. Emit explicit runtime product dependencies for referenced textures, fields, weights, and caches.
5. Separate build-time training provenance from runtime dependencies. Runtime rendering must not need the training level JSON.
6. Compute generation identity over canonical metadata and all payload/dependency identities.

Replace raw concatenated hashing with a role-labelled, length-delimited canonical descriptor. Include source digests, channel/color interpretation, height quantization/conversion, preprocessing versions/settings, calibration, and latent/head interfaces. A source-file hash alone does not describe the representation consumed on the GPU.

Replace fixture-size checks with checked size arithmetic, valid dimensions, correct mip chains, tensor/interface agreement, section bounds/non-overlap, supported formats, and finite values. Version the new container distinctly from existing formats.

Do not weaken compatibility checks when removing hardcoded paths. Report the incompatible field and expected/actual values.

## Runtime binding and atomic publication

Replace controller-wide learned-material/appearance slots with per-profile/material resolved bindings. Maintain a dependency map keyed by asset identity and generation.

A batch key includes profile, model bundle, latent producer, material binding, and kernel family. Equal architecture does not imply equal weights or features.

Build an immutable generation containing all model buffers, learned fields, profile constants, policy descriptors, and derived records. Validate and certify it before publication. Never replace a feature texture or weight buffer independently beneath an active certified generation.

Retain the current strict requested-policy-first enforcement, packet-update invalidation, end-of-render suppression barrier, and scene-subscription lifetime fix.

A previous generation may remain visible during reload only while it remains a complete immutable snapshot compatible with the current strict execution policy. Otherwise suppress it.

| Changed dependency | Invalidation requirement |
|---|---|
| Height interpretation, scale/reference, UV mapping | Geometry bounds and affected face/view caches |
| Representation, LOD, or coverage policy | Proxy selection, coverage, resolve regions, repair inventory |
| Tensors or latent producer | Dependent decoder binding/resources and generation certification |
| Feature dimensions/mapping | Footprint and material/proxy derived state |
| View/projection/sample configuration | Per-view and sample-dependent records |
| Repair capability/continuity group | Producer inventory and paired repair plan |

A broad generation revision is an acceptable first implementation. Optimize reuse only after the dependency model is correct.

## Strict neural-only requirements

Strictness is an execution policy independent of asset identity or lighting-estimator selection.

### Allowed operations

- Fixed bounded descriptor acquisition for neural inference.
- Neural hit prediction and a bounded set of neural candidate hypotheses.
- Bounded local validation of predicted candidates.
- Explicitly supported analytic shell/proxy/repair representations, with honest record semantics.
- Same-estimator uncached evaluation when an optimization cache is unavailable.

### Prohibited operations

- Cell traversal or ray stepping to search for intersections.
- Ordered height-crossing searches, including current nearest-hit recovery.
- Baseline/hybrid fallback on low confidence, a miss, incompatible data, or unsupported resources.
- Implicit selection of a different trained model through a default branch.
- Metadata authorizing arbitrary code or declaring an unsafe implementation strict.
- Treating an analytic repair result as a validated neural surface hit.

### Three certification conditions

1. **Kernel:** the actual shader product/variant's reachable strict path contains no prohibited search or fallback.
2. **Binding:** the selected model, source resources, feature producer, and profile are mutually compatible.
3. **Generation:** submitted packets and resource identities match the certified immutable generation.

A metadata boolean such as `noMarch=true` is not proof. Keep the gem-owned product/variant allowlist, but audit complete reachable behavior behind each entry.

Retain actual backend restrictions until supported deliberately. Asset metadata cannot authorize unsupported instancing, hit reuse, sample configurations, or pipeline behavior.

| Failure | Strict response |
|---|---|
| Missing model / unknown interface | Reject or suppress incompatible generation |
| Source, profile, or latent mismatch | Report exact mismatch; no model substitution |
| No validated predicted hit | Documented miss or explicitly permitted analytic repair |
| Missing optimization cache | Same certified estimator or conservative preparation |
| Pending reload | Retain only a still-valid complete prior generation |
| Incompatible packet/product | Revoke certification and suppress submission |
| Unsupported backend | Explicit unavailable status; no marching |
| Request to leave strict mode | Change execution policy only through explicit configuration |

Fixed descriptor probes used as network inputs and bounded local Newton validation are distinct from an ordered crossing search. Preserve that distinction in code review and naming.

## Shader data and footprint migration

Move all learned tensors, thresholds, cache tile mappings, source dimensions, normalization/calibration values, and fixed content hashes into project-owned products. Audit directional/angular weights as well as Tower-named files.

Use one resolved descriptor across depth, forward shading, normal-only probes, learned material, proxy acquisition, directional features, and compute preparation.

Track source-height dimensions separately from learned-field dimensions. Include source-to-feature UV/mip mapping and trained normalization. Replacing one fixed atlas size with the height texture size is insufficient.

Preserve migrated input ordering and arithmetic initially. Changing normalization with unchanged weights is a model change.

Remove content-sized addressing specializations and retain exact generic NPOT behavior, including negative wrapping and clamp handling.

For legacy packed caches, either define an explicit optional bounded format with asset-owned layout or retire the route through an explicit migration. No content tile tables should remain in gem source under renamed symbols.

## Repair, coverage, LOD, derivatives, and MSAA

Replace overloaded `maxCells` sentinel values with explicit policies. A legacy marcher cell budget must not select strictness, LOD, repair, or coverage.

Replace model-number wall/opening classifications with validated representation and repair capabilities. Update CPU producer inventory, compute preparation, depth record writers, forward record consumers, and repair together.

Current MSAA near-wall specialization encodes model 4, while its NoMSAA path uses the material model. Remove that content specialization without changing sample semantics. CPU inventory must recognize the same capability that the actual shader implements.

Separate neural displaced surface, authored plane, analytic shell, learned proxy, repair donor eligibility, and continuity group. Sharing a model family alone must not permit cross-face repair.

Preserve:

- Conservative envelopes from geometry and validated displacement extrema, not neural confidence.
- Valid bounds for all supported decoder outputs, or constrained outputs within the declared domain.
- Generic conservative fallback when optimized bounds are unavailable.
- Unknown producer capability treated conservatively, never as proof of no donors.
- Paired classify/fill regions from one sealed execution plan.
- Paired full-screen repair when regional coverage is not established.
- Defined initialization for every scratch sample that fill may read.
- Foreground depth rejection and donor/owner identity.
- Consistent publication of repaired hit, depth, UV, representation, and explicit gradients.
- Separate MSAA sample ownership; no new cross-sample shading/cache reuse.
- Existing conservative-depth and interpolation semantics.

If baseline attachment declarations are invalid, rebuild the pair; full-screen scheduling alone does not repair an invalid frame graph.

The reference-resolution LOD convention must become explicit. Preserve its current behavior in migrated project metadata first; adopting a new viewport-derived quality policy is a separate behavior change.

### Preserve center lighting state

Keep the point-4 structure:

1. Normal-only probes use read-only model metadata and local query state.
2. Restore center UV.
3. Run full center surface acquisition last.
4. Create one center lighting receiver for directional and spiral queries.
5. Keep receiver state bound to that invocation's profile/model generation.

Never let generic height/normal helpers publish receiver features or prefixes.

Prefix factoring is interface-specific. The existing shared input 0–7 and proxy-light input 0–10 prefixes remain valid only for matching interfaces. Do not split through light-dependent terms or apply ReLU before completing light-dependent accumulation.

No cache crosses samples, model generations, repaired ownership, or center/probe boundaries.

## Implementation order

Each stage should explicitly list remaining temporary content dependencies. Do not call the migration complete while a renamed global fallback remains.

Existing publication protections and atomic binding of each model/field/profile bundle are prerequisites for stages 4 and 5. Stage 7 completes and audits invalidation and certification; it does not introduce generation consistency for the first time. No intermediate stage may publish mixed resources.

| Stage | Deliverable | Exit condition |
|---|---|---|
| 1 | Strict execution contract and nearest-crossing-search exclusion | All reachable strict intersection paths use supported inference, local validation, or declared analytic behavior |
| 2 | Generic profile/model schema, validated products, dependency identities | Self-contained synthetic products process without any existing test-project assets |
| 3 | Project-owned extraction/conversion tooling and bundles | Existing tensors, calibration, thresholds, fields, and layouts represented outside gem source |
| 4 | Per-profile/material resource binding | Unrelated learned materials coexist in one component without global-slot assumptions |
| 5 | Generic shader descriptors and data-backed tensors | Production shaders contain no trained content values or fixture layouts |
| 6 | Explicit geometry/repair/LOD policies | CPU, compute, depth, forward, and repair agree without semantic model IDs or sentinel modes |
| 7 | Complete generation publication and invalidation | Reload cannot mix model, feature, and profile generations; strict submission stays enforced |
| 8 | Repository cleanup and project conversion | Fixtures/recipes moved, temporary adapters removed, gem usable in independent project |
| 9 | Architectural and correctness regression coverage | New content integration requires zero gem source changes |

The sequence can use temporary explicit adapters, but they must have removal criteria. Keep project-specific ID-to-asset conversion outside the gem from the outset.

Structural extraction should preserve existing trained data and need not retrain every model. Correcting ordered strict recovery can require additional predictor work; track it separately rather than hiding it in the data migration.

Defer further content-specialized optimization until these contracts are established.

## Project migration and serialization

A project-owned converter should:

- Map old numeric model selectors to bundle references.
- Extract trained tensors, thresholds, layout tables, and calibration.
- Record latent producer/head compatibility.
- Convert sentinel modes to explicit policies.
- Update material/component bindings.
- Preserve established source-processing contracts and quality settings.

Use versioned component/material serialization and actionable errors for unmigrated bindings. A narrowly versioned old-format reader may be temporary; it must not invent source bindings from project paths.

Move content-specific training scripts/recipes, integration fixtures, and golden outputs to the consuming project or a separate explicit integration repository. Generic tools may remain only with supplied manifests and no content-specific defaults.

Update source/package/install manifests so removed generated headers and fixtures are no longer required to build or process the gem.

## Acceptance criteria for later implementation

These are proposed checks, not completed tests or measurements.

### Repository independence

- The gem builds and its generic assets process in a project containing none of the existing test-level assets.
- No production gem code constructs paths into a consuming project's content tree.
- No trained tensors, source/latent content hashes, or fixture tile tables remain embedded in production source.
- Generic tests are self-contained. Project integration fixtures do not become implicit build dependencies.
- Audit dependency boundaries positively; do not rely only on a blacklist of current asset names.

### Data and compatibility

- Different source dimensions, learned-field resolutions, mip counts, and addressing modes work through metadata.
- Two unrelated profile/model/material bindings coexist in the same component.
- Moving source files while preserving valid asset references does not break loading.
- Unknown interfaces, invalid tensor sections, malformed dimensions, and incompatible resources are rejected before GPU use.
- Wrong source interpretation, profile calibration, or latent producer yields a specific expected/actual diagnostic.
- Changes to relevant conversion/preprocessing settings invalidate compatibility.

### Strict execution and lifecycle

- Complete strict shader call/dependency inspection finds no ordered crossing search or generic/hybrid marcher.
- Inference misses and unavailable resources never authorize another model or marcher.
- Rejected strict transitions, failed reloads, and packet rebuilds without update notifications cannot re-enable uncertified draws.
- Published generations contain mutually consistent weights, fields, descriptors, and packet options.
- Missing optimization caches preserve the same estimator and execution policy.
- The scene-subscription lifetime fix and final submission barrier remain effective.

### Geometry and rendering correctness

- Conservative bounds include all permitted displaced/proxy geometry.
- Regional and paired full-screen repair preserve initialization, coverage, ownership, and depth ordering.
- Material/repair boundaries preserve explicit gradient provenance and LOD behavior.
- MSAA samples retain their own hit, repair, and gradient state.
- AA normal probes cannot overwrite center features, frame, view state, or receiver prefixes.
- Migrated models preserve their declared feature ordering, normalization, and sampling semantics.
- Any grazing-coverage change caused by removing ordered recovery is documented and addressed explicitly, not described as automatically equivalent.

### Final demonstration

An unrelated project-owned asset is exported, trained, built, and rendered in strict neural-only mode through the new contracts, without changing SILPOM source. Its model can be replaced or hot-reloaded without mixing generations or activating marching.

## Suggested first actions for Claude

1. Read this document and compare the current checkout with the baseline.
2. Reinspect strict recovery and scene-subscription state before making claims about what is fixed.
3. Enumerate remaining content-dependent production code, tools, fixtures, and serialized selectors.
4. Agree the profile/model interfaces and strict failure semantics before restructuring bindings.
5. Implement in the staged order only after receiving implementation authorization; keep each stage reviewable.
6. Report source-supported guarantees separately from future test results and performance hypotheses.

No performance improvement or numerical/visual equivalence is claimed by this handoff.

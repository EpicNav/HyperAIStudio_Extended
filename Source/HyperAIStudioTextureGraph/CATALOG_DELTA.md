# texture_graph catalog delta

`HyperAIStudioTextureGraph` stays unloaded until `HyperAIStudioCapabilityPackCatalog.generated.inl` contains this
cohort. Feed the following to `Plan/CapabilityUnion/build_capability_pack_registry.py` and regenerate.
The tool names, cohort, admission state and safety classes must match exactly, or the module is silently skipped.

## Prerequisites (new)

| Id | Kind |
|---|---|
| `plugin.TextureGraph` | Plugin |
| `module.TextureGraph` | Module |
| `probe.texture_graph_engine` | Probe |

## Pack

- Id `texture_graph`, Tier `Optional`, AdmissionState `SourceCandidate`
- `bContainsExternalEffects = false`: export writes texture assets under `/Game`, so it is Edit
- DependsOnPackIds `shared_foundation`
- AtomicCohortIds `cohort.source.hyperaistudiotexturegraphtoolset.v1`
- ToolNames, in this order: `hyper_texture_graph_inspect`, `hyper_texture_graph_apply_plan`, `hyper_texture_graph_validate`
- Requirement group `texture_graph_plugin`: AllOf, blocking, `plugin.TextureGraph`, `module.TextureGraph`
- Requirement group `texture_graph_backend`: AllOf, blocking, `probe.texture_graph_engine`

## Tools

All three use PackId `texture_graph`, the cohort above, AdmissionState `SourceCandidate`, and
`bMayCauseExternalEffects = false`.

| Tool | AllowedSafetyClasses |
|---|---|
| `hyper_texture_graph_inspect` | Read |
| `hyper_texture_graph_apply_plan` | Edit |
| `hyper_texture_graph_validate` | Read |

## Counts that move with the regeneration

- `SourceArtifactCount` 109 → 112 (generated)
- `FHyperAIStudioCapabilityRuntimeIndex::MaxRuntimeToolBindings` is already 112
- Test constants pinned to the old catalog, to bump in the same change:
  - `HyperAIStudioCapabilityPackRegistryTests.cpp`: 109 at lines 109 and 155
  - `HyperAIStudioNativeReadToolsetTests.cpp`: `CurrentCatalogToolCount`, `CurrentSourceCandidateToolCount`,
    `CurrentDevImplementedToolCount`

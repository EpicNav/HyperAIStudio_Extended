# HyperAIStudioCharacter static freeze

Date: 2026-08-15

Status: **STATIC FREEZE**. This isolated source-candidate pack is ready for the parent-owned catalog regeneration and later centralized UE 5.8 build. No UBT, UAT, packaging, central registry, descriptor, plan, or test-project file was changed by this pack task.

## Frozen identity

- Module: `HyperAIStudioCharacter` (`Editor`, parent wires `LoadingPhase=None`)
- Pack: `character`
- Atomic cohort: `cohort.source.hyperaistudiocharactertoolset.v1`
- Qualified toolset: `HyperAIStudioCharacter.HyperAIStudioCharacterToolset`
- Exactly three `AICallable` functions:
  - `hyper_character_inspect`
  - `hyper_character_apply_plan`
  - `hyper_character_validate`

## Frozen behavior

- All nine UE 5.8 `MetaHumanGenerator.MetaHumanToolset` functions are exact Epic delegates; none is reimplemented.
- The unique HyperAI surface is limited to exact already-loaded top-level MetaHuman character package identity, persisted/volatile seals, and detached value-only health validation.
- Asset Registry access is exclusively `TryGetAssetPackageData(..., true)`; `Unknown` is incomplete and fail-closed.
- Input container and nested-string bounds are checked before copies or materialization.
- The base adapter never loads, opens, searches, scans, generates, exports, writes files, invokes cloud/account behavior, or links MetaHuman/NoRedist implementation modules.
- A dry-run cannot call pure `TypedArtifactExecutor::Prepare` until complete loaded semantic state has been projected. The base adapter therefore returns `character_semantic_projection_backend_required` after identity CAS.
- Every non-dry request returns stable `character_mutation_backend_required` before UObject resolution, with zero physical effects, no staging, and no submission.
- The generated `character` requirement remains blocking/core-owned; the adapter descriptor declares no non-blocking requirement group.
- Registration is class-atomic through `FHyperAIStudioCapabilityRuntimeIndex`, with the typed adapter owned through the trusted execution facade.

## Static sweep

- `AICallable` count: `3`
- Production Asset Registry query sites: `1`; compile-time API pin: `1`
- Blocking requirement IDs in `ApplicableNonBlockingRequirementGroupIds`: `0`
- Synchronous load, Asset Registry scan/search, create/save/transaction/stage/submit API sites: `0`
- Braces balanced and all files end in a newline.
- Packaged/runtime tests are intentionally deferred to the parent-owned central build after all source writers freeze.

## Frozen file hashes

| File | SHA-256 |
|---|---|
| `HyperAIStudioCharacter.Build.cs` | `f83a1da6231f0b3588f5f8cd44b43dd9000b75c3f6d5905f2dfcbc00504e505d` |
| `Private/HyperAIStudioCharacterModule.cpp` | `9d22f77ad3f5d0ebba5bfa962e3a2d4d7ead352bb038f20a93da29089a43e6d1` |
| `Private/HyperAIStudioCharacterToolset.cpp` | `435df9791bd750e764069242bba53cb095c21572802c2611b38facf8d27c7b62` |
| `Private/HyperAIStudioCharacterToolset.h` | `cd738368ba49e20b426397024f0f2d84337e29a4b416bf766c104eb2eecf8483` |
| `Private/Tests/HyperAIStudioCharacterToolsetTests.cpp` | `3da267b9364b59f01dfb3d476f5f33055a11aae713551ae551d8577e961a45a1` |
| `Private/Tests/HyperAIStudioCharacterUE58ApiAudit.cpp` | `7fa9988cc44f37e86a508f2aa1a8637471834e1a2001327e7ac4bf90f1d1833f` |

Any source change invalidates this freeze and its hashes.

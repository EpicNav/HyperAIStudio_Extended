# STATIC FREEZE — pack 18 Physics / Chaos

Date: 2026-08-15
Scope root: `Plugin/Source/HyperAIStudio/Source/HyperAIStudioPhysics/**`
State: SourceCandidate, development-only, zero-effect mutation implementation.

## Exact module and ownership contract

- Module: `HyperAIStudioPhysics`
- Required central descriptor: `Type=Editor`, `LoadingPhase=None`
- Build dependencies, exact: `Core`, `CoreUObject`, `Engine`, `AssetRegistry`, `HyperAIStudio`, `PhysicsCore`, `ToolsetRegistry`
- No hard link: `PhysicsAssetEditor`, `ChaosClothAssetEditor`
- Pack: `physics_chaos`
- Cohort: `cohort.source.hyperaistudiophysicstoolset.v1`
- Qualified toolset: `HyperAIStudioPhysics.HyperAIStudioPhysicsToolset`
- Probe: `probe.physics_asset`
- Blocking groups, centrally owned: `physics_plugin`, `physics_backend`
- Adapter `ApplicableNonBlockingRequirementGroupIds`: exact empty array
- Owner seam only: `PublishOptionalToolset`, `RegisterOwnedToolsetClass`, `UnregisterOwned`, `WithdrawOptionalToolset`
- Adapter/probe seam only: `RegisterAdapter`, `RegisterLiveProbe`, `PublishLiveProbeExact`, corresponding unregister calls

## Exact atomic callable matrix

| Callable | Safety | Variant | Production behavior |
|---|---|---|---|
| `hyper_physics_inspect` | Read | `loaded_exact_physics_asset_snapshot.v1` | Loaded exact `UPhysicsAsset` only; bounded public-value snapshot; no load/open/scan |
| `hyper_physics_apply_plan` | Edit | `guarded_settings_patch_preflight.v1` | Dry-run: deep immutable clone plus pure public `FHyperAIStudioTypedArtifactExecutor::Prepare`; non-dry and adapter: zero effect, `bounded_compile_or_simulation_backend_required` |
| `hyper_physics_validate` | Read | `independent_physics_value_validation.v1` | Detached UObject-free structural validator; `simulation_ready` always requires external simulation evidence |

Exact AICallable count is three. No Store, Claim, Service, trusted prepare/stage, submit, execution host, transaction, mutation, rebuild, compile, simulation, save, or fresh-success implementation is present.

## DTO identities and schema fingerprints

| Role | Type id | Schema fingerprint |
|---|---|---|
| Inspect request | `hyperai.payload.physics.inspect.v1` | `sha256:89d5be7e5a0f5735b1539b3ab82aa17d633592304aeb622d80f888aa6059b6db` |
| Settings patch | `hyperai.payload.physics.settings_patch.v1` | `sha256:42f0859344b1250985daca316e06ef16ff917c471d17a09bf5859937bb971f2d` |
| Validate request | `hyperai.payload.physics.validate.v1` | `sha256:6558f47b062a1131cb5c08c74ba84d19545b06898b3494f1adef8f7d86294ece` |
| Inspect result | `hyperai.result.physics.inspect.v1` | `sha256:4ec28ccdc8fd706040252106ee92eee9f61028b8616c312cd1f8a5a6d24e623b` |
| Blocked mutation result | `hyperai.result.physics.mutation-blocked.v1` | `sha256:0b72005e3f20a4a0dd692bab1797ba3429b2a49a1d17adfa965737ccaa59de76` |
| Validate result | `hyperai.result.physics.validate.v1` | `sha256:26b3fb8235589526504be81fa7e5e45cc2d50a9cf4cf91616f07cad7441d71c6` |

## Epic-equivalence boundary

- Exact native delegates: 23 total — all 17 `PhysicsToolsets.PhysicsAssetToolset` callables and all 6 `ChaosClothAssetToolset.ChaosClothAssetToolset` callables.
- Exact adjacent Python delegates: 5 — collision-channel read, skeletal-mesh physics-asset get/assign, static-mesh convex generation/removal.
- Native review authority: `epic-ue5.8-native-aicallable-access-2026-08-15`, records `sha256:bcce323ed62e3b63a819178c29b8922e6ffd23b109708c9b3bcefe82b4acab88`.
- Python review authority: `epic-ue58-python-access-2026-08-15`, records `sha256:7fcb3857d4d56bc7aa9732322a757bf8009c176f1a61e4b3fb8441806e32ed10`.
- Closed missing cases only: exact loaded CAS snapshot, independent structural validation, and preflight for collision profile/enabled, simulate flag, RBAN solver settings, and constraint collision-disable.
- Explicitly delegated/excluded from HyperAI edits: asset creation, body/shape CRUD, body mode/mass, constraint CRUD/limits, cloth operations, skeletal/static collision helpers.
- Explicitly incomplete: level-set, ML level-set, skinned level-set, skinned-triangle geometry serialization; compile/simulation/save/fresh proof.

## Bounds and revision rules

- Bounds are checked before iterating/materializing public containers: bodies 256, constraints 512, disabled pairs 4096, shapes/body 256, convex vertices 4096, convex indices 24576.
- Production Asset Registry access is exactly one `TryGetAssetPackageData(PackageName, Data, true)` call. `Unknown` is fail-closed and is never absence.
- Target resolution is loaded-only `FSoftObjectPath::ResolveObject`; exact class/path, `RF_WasLoaded`, clean package, nonzero saved hash, and disk size are required for persisted CAS admission.
- Persisted revision and volatile observation fingerprints are separate. Volatile sealing retains the bounded current value projection when dirty/unknown evidence prevents persisted promotion.
- Paging cursors require a complete persisted revision; no incomplete snapshot receives a cursor.

## Frozen files and SHA-256

The freeze file itself is intentionally not self-hashed here; its final hash belongs in the parent integration handoff.

| File | SHA-256 |
|---|---|
| `HyperAIStudioPhysics.Build.cs` | `02410759356a0893589fad9f837009b1eada00664c9e6f44c40a072f1b7cf7b0` |
| `Private/HyperAIStudioPhysicsModule.cpp` | `4cc982da3b8b26501e06e4ea4f9e5220b4c284c5aa567c24d1a8e5e750c2790d` |
| `Private/HyperAIStudioPhysicsToolset.cpp` | `e7d27c929a4ed06497c4801c77d22acaf7c34e77092edc92b57e2af9881879fb` |
| `Private/HyperAIStudioPhysicsToolset.h` | `1b944cb306486babed3a6fc1e8d34fe182e59591dde95cc7e445a6bc67d8cf48` |
| `Private/Tests/HyperAIStudioPhysicsToolsetTests.cpp` | `4d912dd4e174234a082f76d3dc5b0437733468cae759c4324c150b0cce5e510f` |
| `Private/Tests/HyperAIStudioPhysicsUE58ApiAudit.cpp` | `af61991cbe100294a4f250add2d6e32d029e38e11af7f561571095bcd48d0b88` |

## Static verification and central hooks still required

Passed locally without writes outside this folder:

- exact three AICallables and identifiers;
- exact 17+6 native and 5 Python delegate counts;
- exactly one production non-blocking package tri-state call;
- forbidden load/scan/save/stage/submit/store/claim routes absent;
- descriptor blocking-group regression (nonblocking list empty);
- installed UE 5.8 public header symbol/signature audit;
- balanced C++/C# delimiters.

Central integration must still:

1. Add the editor module descriptor with `LoadingPhase=None`; this slice deliberately makes no `.uplugin` edit.
2. Change the generated registry cohort from planned to the exact SourceCandidate cohort and attach exact three-tool source evidence; this slice makes no Plan/catalog edit.
3. Keep both physics requirement groups blocking and keep their authority out of the adapter descriptor.
4. Ensure the central adapter validator accepts the established `hyperai.result.*` result-id convention (the current shared validator appears to apply the request-only `hyperai.payload.*` prefix check to result ids).
5. Run UHT/UBT plus the five Physics automation tests after central module integration. No build/UAT claim is made by this isolated freeze.

Any change to a frozen code/test file invalidates the recorded file hash and requires a new freeze.

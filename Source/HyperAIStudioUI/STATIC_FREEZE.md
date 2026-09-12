# HyperAIStudioUI — STATIC FREEZE

Freeze status: **STATIC FREEZE**. This source candidate is closed for central review. No UBT, UAT, packaging, test-project, generated-output, `.uplugin`, shared-core, central-module, or CapabilityUnion mutation was performed.

## Frozen identity and authority

| Contract | Exact value |
|---|---|
| Module | `HyperAIStudioUI` |
| Module type / loading phase | `Editor` / `None` |
| Pack | `ui_slate_mvvm` |
| Atomic source cohort | `cohort.source.hyperaistudiouitoolset.v1` |
| Qualified toolset | `HyperAIStudioUI.HyperAIStudioUIToolset` |
| Adapter | `adapter.ui.preparation-only.ue58` |
| Edit variant | `ui.apply_edit_plan.v1` |
| Destructive variant | `ui.apply_destructive_plan.v1` |
| Payload | `hyperai.payload.ui.compound-plan.v1` |
| Result | `hyperai.result.ui.compound-plan.v1` |
| Blocking prerequisite groups | `ui_plugin`, `ui_backend` |
| Prerequisite leaves | `module.UMG`, `plugin.ModelViewViewModel` |
| Live probe | `probe.widget_blueprint_editor` |
| Non-blocking descriptor groups | exact empty set |
| Non-dry blocker | `staged_backend_required` |

The atomic manifest is exactly `hyper_ui_inspect`, `hyper_ui_apply_plan`, and `hyper_ui_validate`. There are exactly three `AICallable` declarations. Registration is gated by the catalog-exact source cohort/development policy and goes only through the central capability runtime index's publication, owned registration, owned rollback, and withdrawal seam.

## Epic delegation matrix — 46/46

Every coordinate below remains an Epic delegate. HyperAIStudioUI neither registers a duplicate name nor invokes the Epic callable. The matrix was machine-compared to `epic_native_access_review.json`: 46 expected, 46 present, zero mismatches.

| # | Epic toolset / callable | Lifecycle | Access | Effect domain | UE 5.8 declaration / implementation |
|---:|---|---|---|---|---|
| 1 | UMGToolSet / CreateWidgetBlueprint | create | edit | widget_blueprint_state | UMGToolSet.h:286 / UMGToolSet.cpp:517 |
| 2 | UMGToolSet / AddWidget | create | edit | widget_blueprint_state | UMGToolSet.h:299 / UMGToolSet.cpp:607 |
| 3 | UMGToolSet / SetNamedSlotContent | edit | edit | widget_blueprint_state | UMGToolSet.h:310 / UMGToolSet.cpp:671 |
| 4 | UMGToolSet / GetWidgets | inspect | read | widget_blueprint_state | UMGToolSet.h:322 / UMGToolSet.cpp:769 |
| 5 | UMGToolSet / GetNamedSlots | inspect | read | widget_blueprint_state | UMGToolSet.h:329 / UMGToolSet.cpp:917 |
| 6 | UMGToolSet / ListWidgetBlueprints | discover | read | widget_blueprint_catalog | UMGToolSet.h:336 / UMGToolSet.cpp:1074 |
| 7 | UMGToolSet / ListWidgetClasses | discover | read | widget_blueprint_catalog | UMGToolSet.h:343 / UMGToolSet.cpp:1107 |
| 8 | UMGToolSet / GetWidgetClassInfo | inspect | read | widget_blueprint_state | UMGToolSet.h:353 / UMGToolSet.cpp:1134 |
| 9 | UMGToolSet / MoveWidget | edit | edit | widget_blueprint_state | UMGToolSet.h:365 / UMGToolSet.cpp:1149 |
| 10 | UMGToolSet / RemoveWidget | delete | destructive | widget_blueprint_state | UMGToolSet.h:373 / UMGToolSet.cpp:1170 |
| 11 | UMGToolSet / RenameWidget | edit | edit | widget_blueprint_state | UMGToolSet.h:382 / UMGToolSet.cpp:1182 |
| 12 | UMGToolSet / ToggleWidgetAsVariable | edit | edit | widget_blueprint_state | UMGToolSet.h:391 / UMGToolSet.cpp:1210 |
| 13 | UMGToolSet / BindToEventProperty | edit | edit | widget_blueprint_state | UMGToolSet.h:410 / UMGToolSet.cpp:573 |
| 14 | UMGToolSet / WrapWidgets | edit | edit | widget_blueprint_state | UMGToolSet.h:425 / UMGToolSet.cpp:586 |
| 15 | UMGToolSet / GetWidgetDescription | inspect | read | widget_blueprint_state | UMGToolSet.h:441 / UMGToolSet.cpp:1369 |
| 16 | UMGToolSet / GetWidgetTreeDepth | inspect | read | widget_blueprint_state | UMGToolSet.h:449 / UMGToolSet.cpp:1416 |
| 17 | UMGToolSet / AddUIComponent | create | edit | widget_blueprint_state | UMGToolSet.h:464 / UMGToolSet.cpp:1225 |
| 18 | UMGToolSet / RemoveUIComponent | delete | destructive | widget_blueprint_state | UMGToolSet.h:474 / UMGToolSet.cpp:1260 |
| 19 | UMGToolSet / MoveUIComponent | edit | edit | widget_blueprint_state | UMGToolSet.h:486 / UMGToolSet.cpp:1277 |
| 20 | UMGToolSet / ReplaceWidgetWithTemplate | edit | destructive | widget_replacement_state | UMGToolSet.h:505 / UMGToolSet.cpp:971 |
| 21 | UMGToolSet / ReplaceWidgetWithNamedSlot | edit | destructive | widget_replacement_state | UMGToolSet.h:518 / UMGToolSet.cpp:1052 |
| 22 | UMGToolSet / ReplaceWidgetWithChild | edit | destructive | widget_replacement_state | UMGToolSet.h:529 / UMGToolSet.cpp:1063 |
| 23 | UMGToolSet / CompileWidgetBlueprint | compile | external | widget_blueprint_compilation | UMGToolSet.h:540 / UMGToolSet.cpp:1298 |
| 24 | MVVMToolset / CreateViewModel | create | edit | mvvm_blueprint_state | MVVMToolset.h:42 / MVVMToolset.cpp:275 |
| 25 | MVVMToolset / AddViewModelProperty | create | edit | mvvm_blueprint_state | MVVMToolset.h:54 / MVVMToolset.cpp:324 |
| 26 | MVVMToolset / ListViewModels | discover | read | mvvm_catalog | MVVMToolset.h:63 / MVVMToolset.cpp:376 |
| 27 | MVVMToolset / ListWidgetViewModels | edit | edit | mvvm_blueprint_state | MVVMToolset.h:72 / MVVMToolset.cpp:403 |
| 28 | MVVMToolset / AddViewModelToWidget | edit | edit | mvvm_blueprint_state | MVVMToolset.h:81 / MVVMToolset.cpp:423 |
| 29 | MVVMToolset / ListWidgetViewBindings | edit | edit | mvvm_blueprint_state | MVVMToolset.h:90 / MVVMToolset.cpp:442 |
| 30 | MVVMToolset / RemoveWidgetViewBinding | delete | destructive | mvvm_widget_binding | MVVMToolset.h:99 / MVVMToolset.cpp:454 |
| 31 | MVVMToolset / CreateViewBinding | create | edit | mvvm_blueprint_state | MVVMToolset.h:122 / MVVMToolset.cpp:482 |
| 32 | MVVMToolset / ListConversionFunctions | discover | read | mvvm_catalog | MVVMToolset.h:130 / MVVMToolset.cpp:521 |
| 33 | SlateInspectorToolset / Snapshot | inspect | read | slate_widget_tree | SlateInspectorToolset.h:91 / SlateInspectorToolset.cpp:167 |
| 34 | SlateInspectorToolset / Observe | runtime | external | slate_observer_runtime | SlateInspectorToolset.h:101 / SlateInspectorToolset.cpp:196 |
| 35 | SlateInspectorToolset / Unobserve | runtime | external | slate_observer_runtime | SlateInspectorToolset.h:106 / SlateInspectorToolset.cpp:213 |
| 36 | SlateInspectorToolset / ListObservers | discover | read | slate_observer_catalog | SlateInspectorToolset.h:112 / SlateInspectorToolset.cpp:218 |
| 37 | SlateInspectorToolset / Screenshot | render | external | slate_image_capture | SlateInspectorToolset.h:118 / SlateInspectorToolset.cpp:254 |
| 38 | SlateInspectorToolset / Click | runtime | external | slate_input_and_window_runtime | SlateInspectorToolset.h:128 / SlateInspectorToolset.cpp:292 |
| 39 | SlateInspectorToolset / Hover | runtime | external | slate_input_and_window_runtime | SlateInspectorToolset.h:133 / SlateInspectorToolset.cpp:308 |
| 40 | SlateInspectorToolset / Type | runtime | external | slate_input_and_window_runtime | SlateInspectorToolset.h:141 / SlateInspectorToolset.cpp:330 |
| 41 | SlateInspectorToolset / PressKey | runtime | external | slate_input_and_window_runtime | SlateInspectorToolset.h:147 / SlateInspectorToolset.cpp:372 |
| 42 | SlateInspectorToolset / SelectOption | runtime | external | slate_input_and_window_runtime | SlateInspectorToolset.h:154 / SlateInspectorToolset.cpp:453 |
| 43 | SlateInspectorToolset / Drag | runtime | external | slate_input_and_window_runtime | SlateInspectorToolset.h:161 / SlateInspectorToolset.cpp:548 |
| 44 | SlateInspectorToolset / Windows | runtime | external | slate_input_and_window_runtime | SlateInspectorToolset.h:167 / SlateInspectorToolset.cpp:618 |
| 45 | SlateInspectorToolset / WaitFor | inspect | read | slate_widget_tree | SlateInspectorToolset.h:174 / SlateInspectorToolset.cpp:673 |
| 46 | SlateInspectorToolset / FillForm | runtime | external | slate_input_and_window_runtime | SlateInspectorToolset.h:180 / SlateInspectorToolset.cpp:749 |

The two MVVM `ListWidget*` callables are deliberately `edit/edit`: their UE 5.8 implementations use RequestView, which creates missing view state. Product capture only reads an already-existing extension/view through public getters and never calls either helper or the deprecated source-migration getter.

## Unique v1 surface

Inspect is game-thread, loaded-only, and hard bounded. Exact targets use the canonical loaded object only; empty scope uses a capped raw-object prefix. No asset load or registry object scan occurs. Disk evidence uses only non-blocking package tri-state lookup with lock-failure enabled, nonzero saved hash, and positive disk size. `Unknown` is lock-unavailable/incomplete and is never converted into absence. Planning additionally requires `RF_WasLoaded`, a clean package, and the exact current `UWidgetBlueprint` class.

The detached persisted projection contains the blueprint, tree, stable widget ids, supported slot layout, supported widget values/styles, animations and binding identities, legacy bindings, and an already-existing MVVM extension/view with count-exact view-model/binding inventory and its four persisted view settings. Arbitrary animation track/channel contents, MVVM events/conditions/conversions/context objects, and deprecated self/none source ambiguity are explicit incomplete evidence. Volatile package/compile observations have a separate fingerprint and are never pageable. Stable pages bind the cursor to both the exact request and fresh persisted fingerprint.

Validation is independent and value-only: it receives no UObject pointer. It checks identity/key uniqueness, field type/finiteness, tree parent closure/cycles/root count, supported layout ranges, visibility, ProgressBar/Slider ranges, accessibility warnings, animation ranges/targets, legacy binding targets, MVVM endpoint/path closure, existing-view presence, persisted settings, and exact MVVM counts.

The compound planner has immutable base/desired snapshots, whole-snapshot CAS, per-element CAS for every non-create operation, deterministic shadow replay, persisted desired fingerprint, semantic payload fingerprint, separately derived safety, effect fingerprint, deep immutable clone verification, and public pure `FHyperAIStudioTypedArtifactExecutor::Prepare` sealing.

| Closed operation | Safety |
|---|---|
| `animation.create` | edit |
| `animation.rename` | edit |
| `animation.delete` | destructive |
| `widget.set_property` | edit |
| `widget.set_style` | edit |
| `binding.create_legacy` | edit |
| `binding.update_legacy` | edit |
| `binding.replace_legacy` | destructive |
| `binding.delete_legacy` | destructive |
| `binding.create_mvvm` | edit |
| `binding.update_mvvm` | edit |
| `binding.replace_mvvm` | destructive |
| `binding.delete_mvvm` | destructive |

Delete and replace are the only separately named destructive forms. Safety is derived from operation kind, rechecked in semantic sealing, bound to one of two descriptor variants, and covered by automation tests. There is no client-controlled downgrade to read/edit.

Non-dry remains intentionally zero-effect. The source candidate mutates no UObject, compiles nothing, saves nothing, reloads nothing, creates no MVVM state, and enters no private typed-artifact lifecycle. It returns `staged_backend_required` after exact fresh re-preparation (or an earlier, more specific truthful blocker). No hard-bounded public UE 5.8 mutation + compile-once + save-once + fresh-verification route was proven.

## Unsupported/delegated matrix

| Area | Frozen behavior |
|---|---|
| All 46 Epic coordinates | Delegate only; never duplicated or invoked |
| Slate live widget inspection | Epic delegate; this pack snapshots authored Widget Blueprint values, not live Slate |
| Slate observer, render, input, form, and window actions | External delegate only; no arbitrary live action surface |
| Unloaded exact target | No load; package tri-state evidence plus explicit `target_not_already_loaded` |
| Registry lock/index unavailable | `Unknown`, incomplete, never absence |
| Dirty, memory-created, or subclass target | Inspectable evidence; planning blocked |
| Unsupported slot class | Typed marker plus incomplete warning |
| Arbitrary MovieScene tracks, sections, channels | Counts/classes only; content delegated and incomplete |
| MVVM helper lists | Edit delegates because of state creation; never used for capture |
| MVVM missing extension/view | No creation; absence or explicit incomplete evidence |
| MVVM self/none deprecated ambiguity | No migration call; explicit incomplete evidence |
| MVVM events, conditions, conversions, resolver/instanced content | Delegated and incomplete when present |
| Generic class/property/reflection dispatch | Absent |
| Script, file, process, network, synchronous load | Absent |
| Mutation, compile, save, reload, fresh execution | Backend not proven; non-dry zero-effect |

## Frozen files and dependencies

Files:

1. `HyperAIStudioUI.Build.cs`
2. `Private/HyperAIStudioUIToolset.h`
3. `Private/HyperAIStudioUIToolset.cpp`
4. `Private/HyperAIStudioUIValueModel.h`
5. `Private/HyperAIStudioUIValueModel.cpp`
6. `Private/HyperAIStudioUICapture.cpp`
7. `Private/HyperAIStudioUIDelegationMatrix.h`
8. `Private/HyperAIStudioUIDelegationMatrix.cpp`
9. `Private/HyperAIStudioUIModule.cpp`
10. `Private/Tests/HyperAIStudioUIToolsetTests.cpp`
11. `Private/Tests/HyperAIStudioUIUE58ApiAudit.cpp`
12. `STATIC_FREEZE.md`

Private dependencies are exactly: `Core`, `CoreUObject`, `Engine`, `AssetRegistry`, `HyperAIStudio`, `ToolsetRegistry`, `UMG`, `UMGEditor`, `UnrealEd`, `SlateCore`, `MovieScene`, `ModelViewViewModel`, and `ModelViewViewModelBlueprint`.

The API audit pins the UE 5.8 public members/getters used for WidgetBlueprint, WidgetTree, panel slots, WidgetAnimation/MovieScene, existing MVVM extension/view/settings/view-model/binding paths, text-source access, loaded/dirty flags, and the non-blocking package tri-state signature. Its fixture proves `Unknown` remains unknown rather than absence.

Static freeze checks:

- Exact Epic JSON comparison: `46 / 46 / 0 mismatches` (`23 UMG + 9 MVVM + 14 SlateInspector`).
- Exactly three AI-callable annotations and three manifest names.
- Exact `ui_slate_mvvm` pack and source cohort; no legacy `ui` pack literal.
- Descriptor non-blocking requirement array is exact empty.
- Production package evidence has one permitted non-blocking tri-state call; the second textual occurrence is the compile-time API pin.
- Forbidden object-registry scans, global asset arrays/search-state queries, MVVM state-creating list helpers, deprecated source migration, generic reflection/export, synchronous loading, live Slate action APIs, private execution lifecycle entrypoints, and facade staging/submission: zero invocation hits in `.h`, `.cpp`, and `.cs`.
- No mutation/compile/save call sites.
- Balanced delimiters and zero trailing whitespace in the frozen code files.
- No build or Unreal automation execution was authorized or run for this pack.

## Required central integration hooks (not applied here)

1. Add the single optional `HyperAIStudioUI` Editor module with `LoadingPhase: None`.
2. Keep registry section 15 pack id `ui_slate_mvvm`; promote its planned cohort to `cohort.source.hyperaistudiouitoolset.v1` only as an exact `SourceCandidate`/development admission for the three frozen tools.
3. Change authoritative `ui_plugin` from `any_of` to `all_of`. This module hard-links both UMG and ModelViewViewModel/ModelViewViewModelBlueprint, so UMG alone is insufficient authority.
4. Before loading the optional module, require the `ModelViewViewModel` plugin enabled and require the `UMG`, `UMGEditor`, `ModelViewViewModel`, and `ModelViewViewModelBlueprint` modules available.
5. Keep both `ui_plugin` and `ui_backend` blocking. Do not place either id in `ApplicableNonBlockingRequirementGroupIds`.
6. Keep `probe.widget_blueprint_editor` blocking/missing and edit/destructive admission false until a real bounded backend, compile/save lifecycle, and fresh verifier are centrally proven.
7. Use only the existing central optional-runtime-index loading/ownership hook; do not add direct toolset-registry ownership elsewhere.

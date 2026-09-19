// Games by Hyper 2026.

#include "HyperAIStudioAnimBlueprintCookbook.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace HyperAIStudio::AnimBlueprint
{
	namespace
	{
		// Split into literals under the compiler's single-literal size limit.
		const TCHAR* const Sections[] = {
TEXT(R"(# Animation Blueprint Cookbook

Written by HyperAIStudio for agents building character animation. Use it with `hyper_anim_blueprint_inspect`,
`hyper_anim_blueprint_apply_plan` and `hyper_anim_blueprint_validate`: plan, apply, then validate after every plan.

## 1. Architecture

- One main Animation Blueprint per skeleton. Gameplay code sets state on the character; the Animation Blueprint
  reads it. Never drive gameplay from the Animation Blueprint.
- Compute each value once per frame, then read it everywhere. `bind_variable` adds that per-frame logic to Event
  Blueprint Update Animation. Lyra goes further with Blueprint Thread Safe Update Animation and Property Access,
  which keep the update off the game thread; move logic there as a project grows.
- Order of the output graph: locomotion state machine, then an upper-body Layered Blend Per Bone (from `spine_01`),
  then an additive aim offset, then a Slot (`DefaultSlot`) for montages, then the output pose. These tools build
  the state machine; add the layering nodes in the Animation Blueprint editor.
- Per-weapon or per-stance animation belongs in Linked Anim Layers (Lyra: the `ALI_ItemAnimLayers` interface), so
  the main graph never changes when a weapon does; each weapon links its own layer class.

## 2. Variables

| Variable | Type | How | Used for |
|---|---|---|---|
| Speed | float | `bind_variable` Speed = speed | idle and move transitions, blend space X |
| IsFalling | bool | `bind_variable` IsFalling = is_falling | jump, fall and land |
| IsCrouching | bool | `bind_variable` IsCrouching = is_crouching | crouch states |
| Direction | float | event graph: `Calculate Direction` (velocity, actor rotation) | strafing blend space Y, -180..180 |
| IsAccelerating | bool | event graph: current acceleration length > 0 | starts and stops that react to input |

Use ground speed (velocity X and Y) for locomotion. Input-based values make the pose lead or lag what the
capsule actually does.
)"),
TEXT(R"(
## 3. Locomotion state machine

States: Idle (sequence, looping), Move (blend space by Speed, and Direction if it is 2D), Fall (fall loop),
Land (sequence played once).

| From | To | Rule | Blend |
|---|---|---|---|
| Idle | Move | `Speed > 10` | 0.2 |
| Move | Idle | `Speed < 5` | 0.25 |
| Idle, Move | Fall | `IsFalling` | 0.1-0.15 |
| Fall | Land | `!IsFalling` | 0.1 |
| Land | Idle | `time_remaining < 0.2` | 0.2 |
| Land | Move | `Speed > 10 && !IsFalling` | 0.15 |

- Always leave a gap between the threshold that enters a state and the one that leaves it (above 10 in, below 5
  out). With one shared threshold the machine flickers between states near it; validate flags this as
  `no_hysteresis`.
- A state that plays once needs a `time_remaining` exit, or its last frame holds (`frozen_pose`).
- The land-to-move exit keeps a moving landing from stopping first.

Plan sketch (one `ops` array, then compile runs automatically):

```json
[
  {"kind":"bind_variable","name":"Speed","value":"speed"},
  {"kind":"bind_variable","name":"IsFalling","value":"is_falling"},
  {"kind":"add_state_machine","name":"Locomotion"},
  {"kind":"add_state","machine":"Locomotion","name":"Idle","asset":"/Game/.../MM_Idle.MM_Idle"},
  {"kind":"add_state","machine":"Locomotion","name":"Move","asset":"/Game/.../BS_Jog.BS_Jog","x_variable":"Speed"},
  {"kind":"add_transition","machine":"Locomotion","name":"Idle","to":"Move","rule":"Speed > 10"},
  {"kind":"add_transition","machine":"Locomotion","name":"Move","to":"Idle","rule":"Speed < 5","value":"0.25"}
]
```
)"),
TEXT(R"(
## 4. Feet that do not slide

- Place each blend space sample at the speed its animation actually travels (its root-motion speed): a walk that
  covers 150 cm/s sits at X = 150. A sample placed off its real speed slides at every other speed.
- Put cycles that blend together in one sync group, with sync markers on foot plants, so blended feet stay in
  phase.
- Between samples, Stride Warping (Animation Warping plugin) adjusts stride length to speed with less sliding than
  scaling play rate. Orientation Warping keeps feet planted while the body strafes.
- Turn in place by rotating the root with the animation instead of spinning the capsule under planted feet.

## 5. Transitions that do not pop

- A 0 s blend snaps; use 0.1-0.25 s for locomotion, 0.1 into landings, 0.2-0.3 into stops. Validate flags zero
  blends (`instant_blend`) and slow ones over 0.6 s (`slow_blend`).
- For large pose changes, add an Inertialization node after the state machine and set those transitions' blend
  logic to Inertialization in the editor. It is cheaper and smoother than crossfading two poses.
- Keep rules mutually exclusive where they share a source state, and set priority order where they cannot be, so
  two transitions never compete in one frame.

## 6. Distance matching and motion matching

- Distance Matching (Animation Locomotion Library plugin) plays starts, stops and pivots by distance travelled or
  remaining instead of time, so a stop animation ends exactly where the capsule stops.
- Motion Matching (Pose Search plugin) replaces much of a locomotion state machine: a Pose Search Schema chooses
  the channels (trajectory and pose), a Pose Search Database holds the animations, and a Motion Matching node picks
  the best frame each update. It suits large animation sets. Set these up in the editor; these tools do not author
  Pose Search assets yet.

## 7. Working with the tools

1. `hyper_anim_blueprint_inspect` with `bLoad`, or `apply_plan` with `bCreate` and `skeleton_path` for a new one.
2. Dry-run `apply_plan`, submit it, poll `hyper_operation_status`; every plan compiles.
3. `hyper_anim_blueprint_validate`: fix errors, then warnings (hysteresis, blends, `never_set`, unreachable).
4. Play in the editor and watch feet and transitions, with the Animation Blueprint debugger on the character.

Not built by the tools yet: linked anim layers, slots, aim offsets, layered blends, inertialization nodes,
Pose Search and retargeting. Build those in the Animation Blueprint editor.
)")};
	}

	FString GetCookbook()
	{
		FString Text;
		for (const TCHAR* Section : Sections)
		{
			Text += Section;
		}
		return Text;
	}

	FString GetCookbookPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT(".hyperai/docs/HyperAIStudio-AnimationCookbook.md"));
	}

	bool EnsureCookbook()
	{
		const FString Path = GetCookbookPath();
		const FString Text = GetCookbook();
		FString Existing;
		if (FFileHelper::LoadFileToString(Existing, *Path) && Existing == Text)
		{
			return true;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}

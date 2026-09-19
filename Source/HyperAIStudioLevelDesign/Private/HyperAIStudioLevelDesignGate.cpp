// Games by Hyper 2026.

#include "HyperAIStudioLevelDesignGate.h"

#include "Components/CapsuleComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TargetPoint.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "GameMapsSettings.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "ImageUtils.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "RenderingThread.h"
#include "ScopedTransaction.h"
#include "TextureResource.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioLevelDesign"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioLevelDesignGate, Log, All);

namespace HyperAIStudio::LevelDesign::Gate
{
	namespace
	{
		const FName PieceTag(TEXT("HyperAILD"));
		constexpr double MaxCoordinate = 1000000.0;
		constexpr double MaxExtent = 100000.0;
		constexpr int32 MaxStairSteps = 64;
		constexpr int32 MaxIssues = 128;
		constexpr int32 MaxPairs = 64;
		constexpr int32 PreviewSize = 768;

		const TCHAR* const BlockRoles[] = {TEXT("floor"), TEXT("wall"), TEXT("platform"), TEXT("pillar"), TEXT("cover_low"), TEXT("cover_high"), TEXT("generic")};
		const TCHAR* const MarkerRoles[] = {TEXT("start"), TEXT("goal"), TEXT("objective"), TEXT("spawn"), TEXT("cover"), TEXT("pickup"), TEXT("checkpoint")};

		template <int32 N>
		bool IsOneOf(const FString& Value, const TCHAR* const (&Options)[N])
		{
			for (const TCHAR* Option : Options)
			{
				if (Value == Option) return true;
			}
			return false;
		}

		template <int32 N>
		FString JoinOptions(const TCHAR* const (&Options)[N])
		{
			TArray<FString> Parts;
			for (const TCHAR* Option : Options) Parts.Add(Option);
			return FString::Join(Parts, TEXT(", "));
		}

		FString FormatVector(const FVector& Value)
		{
			return FString::Printf(TEXT("%g,%g,%g"), FMath::RoundToDouble(Value.X * 10.0) / 10.0,
				FMath::RoundToDouble(Value.Y * 10.0) / 10.0, FMath::RoundToDouble(Value.Z * 10.0) / 10.0);
		}

		bool ParseVector(const FString& Text, FVector& Out)
		{
			TArray<FString> Parts;
			Text.ParseIntoArray(Parts, TEXT(","));
			if (Parts.Num() != 3) return false;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				double Value = 0.0;
				if (!LexTryParseString(Value, *Parts[Axis].TrimStartAndEnd()) || !FMath::IsFinite(Value) || FMath::Abs(Value) > MaxCoordinate)
				{
					return false;
				}
				Out[Axis] = Value;
			}
			return true;
		}

		bool IsValidName(const FString& Name)
		{
			if (Name.IsEmpty() || Name.Len() > 48) return false;
			for (const TCHAR Char : Name)
			{
				if (!FChar::IsAlnum(Char) && Char != TEXT('_') && Char != TEXT('-')) return false;
			}
			return true;
		}

		/** One piece's parameters, as stored in its actors' tags. */
		struct FSpec
		{
			FString Name;
			FString Shape;
			FString Role;
			FVector Location = FVector::ZeroVector;
			FVector Size = FVector::ZeroVector;
			double Yaw = 0.0;
		};

		FString TagValue(const AActor& Actor, const TCHAR* Key)
		{
			const FString Prefix = FString(TEXT("HyperAILD.")) + Key + TEXT("=");
			for (const FName& Tag : Actor.Tags)
			{
				const FString Text = Tag.ToString();
				if (Text.StartsWith(Prefix)) return Text.RightChop(Prefix.Len());
			}
			return FString();
		}

		bool ReadSpec(const AActor& Actor, FSpec& Out)
		{
			Out.Name = TagValue(Actor, TEXT("Name"));
			Out.Shape = TagValue(Actor, TEXT("Shape"));
			Out.Role = TagValue(Actor, TEXT("Role"));
			LexTryParseString(Out.Yaw, *TagValue(Actor, TEXT("Yaw")));
			ParseVector(TagValue(Actor, TEXT("Location")), Out.Location);
			ParseVector(TagValue(Actor, TEXT("Size")), Out.Size);
			return !Out.Name.IsEmpty() && !Out.Shape.IsEmpty();
		}

		TArray<FName> TagsFor(const FSpec& Spec)
		{
			return {PieceTag,
				FName(*(TEXT("HyperAILD.Name=") + Spec.Name)),
				FName(*(TEXT("HyperAILD.Shape=") + Spec.Shape)),
				FName(*(TEXT("HyperAILD.Role=") + Spec.Role)),
				FName(*(TEXT("HyperAILD.Location=") + FormatVector(Spec.Location))),
				FName(*(TEXT("HyperAILD.Size=") + FormatVector(Spec.Size))),
				FName(*FString::Printf(TEXT("HyperAILD.Yaw=%g"), Spec.Yaw))};
		}

		/** Piece actors grouped by name, each group sorted by label so step order is stable. */
		TMap<FString, TArray<AActor*>> GroupPieces(UWorld& World)
		{
			TMap<FString, TArray<AActor*>> Groups;
			for (TActorIterator<AActor> It(&World); It; ++It)
			{
				AActor* Actor = *It;
				if (!IsValid(Actor) || !Actor->ActorHasTag(PieceTag)) continue;
				const FString Name = TagValue(*Actor, TEXT("Name"));
				if (!Name.IsEmpty()) Groups.FindOrAdd(Name.ToLower()).Add(Actor);
			}
			for (TPair<FString, TArray<AActor*>>& Group : Groups)
			{
				Group.Value.Sort([](const AActor& A, const AActor& B) { return A.GetActorLabel() < B.GetActorLabel(); });
			}
			Groups.KeySort(TLess<FString>());
			return Groups;
		}

		struct FParsedOp
		{
			FString Kind;
			FSpec Spec;
			bool bHasYaw = false;
		};

		int32 StairSteps(const FVector& Size, const float MaxStepHeight)
		{
			// A tenth under the limit, so a step never sits exactly on the edge of what the movement allows.
			return FMath::Max(1, FMath::CeilToInt(Size.Z / FMath::Max(1.0f, MaxStepHeight * 0.9f)));
		}

		bool ParseOp(const FHyperAILevelDesignOp& Op, FParsedOp& Out, FString& OutError)
		{
			Out.Kind = Op.Kind;
			Out.Spec.Name = Op.Name;
			Out.Spec.Role = Op.Role;
			if (!IsValidName(Op.Name))
			{
				OutError = TEXT("name must be 1-48 letters, digits, _ or -.");
				return false;
			}
			if (!Op.Value.IsEmpty())
			{
				if (!LexTryParseString(Out.Spec.Yaw, *Op.Value.TrimStartAndEnd()) || !FMath::IsFinite(Out.Spec.Yaw) || FMath::Abs(Out.Spec.Yaw) > 360.0)
				{
					OutError = TEXT("value is the yaw in degrees, from -360 to 360.");
					return false;
				}
				Out.bHasYaw = true;
			}
			const bool bAdd = Op.Kind.StartsWith(TEXT("add_"));
			if ((bAdd || Op.Kind == TEXT("move")) && !ParseVector(Op.Location, Out.Spec.Location))
			{
				OutError = TEXT("location must be \"x,y,z\" in centimetres.");
				return false;
			}
			if (Op.Kind == TEXT("add_block") || Op.Kind == TEXT("add_ramp") || Op.Kind == TEXT("add_stairs"))
			{
				if (!ParseVector(Op.Size, Out.Spec.Size) || Out.Spec.Size.GetMin() < 1.0 || Out.Spec.Size.GetMax() > MaxExtent)
				{
					OutError = TEXT("size must be \"x,y,z\" with each from 1 to 100000 cm.");
					return false;
				}
			}
			if (Op.Kind == TEXT("add_block"))
			{
				Out.Spec.Shape = TEXT("block");
				if (Out.Spec.Role.IsEmpty()) Out.Spec.Role = TEXT("generic");
				if (!IsOneOf(Out.Spec.Role, BlockRoles))
				{
					OutError = TEXT("add_block role must be one of ") + JoinOptions(BlockRoles) + TEXT(".");
					return false;
				}
				return true;
			}
			if (Op.Kind == TEXT("add_ramp") || Op.Kind == TEXT("add_stairs"))
			{
				Out.Spec.Shape = Op.Kind.RightChop(4);
				Out.Spec.Role = Out.Spec.Shape;
				return true;
			}
			if (Op.Kind == TEXT("add_marker"))
			{
				Out.Spec.Shape = TEXT("marker");
				if (!IsOneOf(Out.Spec.Role, MarkerRoles))
				{
					OutError = TEXT("add_marker role must be one of ") + JoinOptions(MarkerRoles) + TEXT(".");
					return false;
				}
				return true;
			}
			if (Op.Kind == TEXT("move") || Op.Kind == TEXT("delete"))
			{
				return true;
			}
			OutError = FString::Printf(TEXT("unknown kind '%s': use add_block, add_ramp, add_stairs, add_marker, move or delete."), *Op.Kind);
			return false;
		}

		/** Checks a piece's shape against the player; notes what it will build. */
		bool CheckShape(const FSpec& Spec, const float MaxStepHeight, const float MaxSlopeDegrees, TArray<FString>& OutNotes, FString& OutError)
		{
			if (Spec.Shape == TEXT("ramp"))
			{
				const double Slope = FMath::RadiansToDegrees(FMath::Atan2(Spec.Size.Z, Spec.Size.X));
				if (Slope > MaxSlopeDegrees - 0.5)
				{
					const double MinLength = Spec.Size.Z / FMath::Tan(FMath::DegreesToRadians(MaxSlopeDegrees - 1.0));
					OutError = FString::Printf(TEXT("ramp %s rises at %.1f degrees but the player walks up at most %.1f; make it at least %.0f cm long."),
						*Spec.Name, Slope, MaxSlopeDegrees, MinLength);
					return false;
				}
				OutNotes.Add(FString::Printf(TEXT("ramp %s: %.1f degree slope."), *Spec.Name, Slope));
			}
			else if (Spec.Shape == TEXT("stairs"))
			{
				const int32 Steps = StairSteps(Spec.Size, MaxStepHeight);
				const double Run = Spec.Size.X / Steps;
				if (Steps > MaxStairSteps || Run < 20.0)
				{
					OutError = FString::Printf(TEXT("stairs %s would need %d steps %.0f cm deep; lengthen them or split the climb."), *Spec.Name, Steps, Run);
					return false;
				}
				OutNotes.Add(FString::Printf(TEXT("stairs %s: %d steps, %.1f cm high and %.1f cm deep."), *Spec.Name, Steps, Spec.Size.Z / Steps, Run));
			}
			return true;
		}

		UStaticMesh* CubeMesh()
		{
			return LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		}

		UMaterialInterface* GridMaterial()
		{
			return LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
		}

		AActor* SpawnCube(UWorld& World, const FSpec& Spec, const FString& Label, const FVector& Center, const FRotator& Rotation, const FVector& Extent)
		{
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transactional;
			const FTransform Transform(Rotation, Center, Extent / 100.0);
			AStaticMeshActor* Actor = World.SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Transform, Params);
			if (!Actor) return nullptr;
			// A registered static component refuses a new mesh, so it is set the way the mesh actor factory does.
			UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
			Mesh->UnregisterComponent();
			Mesh->SetStaticMesh(CubeMesh());
			Mesh->SetMaterial(0, GridMaterial());
			Mesh->RegisterComponent();
			Actor->SetActorLabel(Label);
			Actor->Tags = TagsFor(Spec);
			Actor->SetFolderPath(TEXT("HyperAI/LevelDesign"));
			return Actor;
		}

		/** Builds a piece from its parameters; returns how many actors it made, or -1. */
		int32 Build(UWorld& World, const FSpec& Spec, const float MaxStepHeight)
		{
			const FRotator Facing(0.0, Spec.Yaw, 0.0);
			const FVector Forward = Facing.Vector();
			const FString Label = TEXT("LD_") + Spec.Name;
			if (Spec.Shape == TEXT("marker"))
			{
				FActorSpawnParameters Params;
				Params.ObjectFlags |= RF_Transactional;
				ATargetPoint* Marker = World.SpawnActor<ATargetPoint>(ATargetPoint::StaticClass(), FTransform(Facing, Spec.Location), Params);
				if (!Marker) return -1;
				Marker->SetActorLabel(Label);
				Marker->Tags = TagsFor(Spec);
				Marker->SetFolderPath(TEXT("HyperAI/LevelDesign"));
				return 1;
			}
			if (Spec.Shape == TEXT("block"))
			{
				return SpawnCube(World, Spec, Label, Spec.Location + FVector(0.0, 0.0, Spec.Size.Z * 0.5), Facing, Spec.Size) ? 1 : -1;
			}
			if (Spec.Shape == TEXT("ramp"))
			{
				// A 20 cm slab tilted up along its length, its top surface running from the start to the full rise.
				constexpr double Thickness = 20.0;
				const double Pitch = FMath::RadiansToDegrees(FMath::Atan2(Spec.Size.Z, Spec.Size.X));
				const FRotator Tilt(Pitch, Spec.Yaw, 0.0);
				const FVector Normal = Tilt.RotateVector(FVector::UpVector);
				const FVector Center = Spec.Location + Forward * (Spec.Size.X * 0.5) + FVector(0.0, 0.0, Spec.Size.Z * 0.5) - Normal * (Thickness * 0.5);
				const double Length = FMath::Sqrt(FMath::Square(Spec.Size.X) + FMath::Square(Spec.Size.Z));
				return SpawnCube(World, Spec, Label, Center, Tilt, FVector(Length, Spec.Size.Y, Thickness)) ? 1 : -1;
			}
			// Stairs: solid steps from the ground up, each no taller than the player can step.
			const int32 Steps = StairSteps(Spec.Size, MaxStepHeight);
			const double Rise = Spec.Size.Z / Steps;
			const double Run = Spec.Size.X / Steps;
			for (int32 Step = 0; Step < Steps; ++Step)
			{
				const double Height = Rise * (Step + 1);
				const FVector Center = Spec.Location + Forward * (Run * (Step + 0.5)) + FVector(0.0, 0.0, Height * 0.5);
				if (!SpawnCube(World, Spec, FString::Printf(TEXT("%s_%02d"), *Label, Step + 1), Center, Facing, FVector(Run, Spec.Size.Y, Height)))
				{
					return -1;
				}
			}
			return Steps;
		}

		bool RunOps(UWorld& World, const TArray<FHyperAILevelDesignOp>& Ops, const float MaxStepHeight, const float MaxSlopeDegrees,
			const bool bWrite, TArray<FString>& OutNotes, int32& OutActorsChanged, FString& OutError)
		{
			TMap<FString, TArray<AActor*>> Groups = GroupPieces(World);
			// Pieces as they will be at each point in the plan, keyed by lower-case name.
			TMap<FString, FSpec> Planned;
			for (const TPair<FString, TArray<AActor*>>& Group : Groups)
			{
				FSpec Spec;
				if (ReadSpec(*Group.Value[0], Spec)) Planned.Add(Group.Key, Spec);
			}
			for (int32 Index = 0; Index < Ops.Num(); ++Index)
			{
				FParsedOp Parsed;
				FString Error;
				auto Fail = [&](const FString& Why)
				{
					OutError = FString::Printf(TEXT("Op %d (%s %s): %s"), Index, *Ops[Index].Kind, *Ops[Index].Name, *Why);
					return false;
				};
				if (!ParseOp(Ops[Index], Parsed, Error)) return Fail(Error);
				const FString Key = Parsed.Spec.Name.ToLower();
				const bool bExists = Planned.Contains(Key);
				if (Parsed.Kind.StartsWith(TEXT("add_")))
				{
					if (bExists) return Fail(TEXT("a piece already has that name; move it, delete it, or pick another name."));
					if (!CheckShape(Parsed.Spec, MaxStepHeight, MaxSlopeDegrees, OutNotes, Error)) return Fail(Error);
					Planned.Add(Key, Parsed.Spec);
					if (bWrite)
					{
						const int32 Built = Build(World, Parsed.Spec, MaxStepHeight);
						if (Built < 0) return Fail(TEXT("the level refused a new actor."));
						OutActorsChanged += Built;
					}
					continue;
				}
				if (!bExists) return Fail(TEXT("no piece placed by this toolset has that name; inspect lists them."));
				if (bWrite)
				{
					for (AActor* Actor : Groups.FindRef(Key))
					{
						World.EditorDestroyActor(Actor, /*bShouldModifyLevel=*/true);
						++OutActorsChanged;
					}
					Groups.Remove(Key);
				}
				if (Parsed.Kind == TEXT("delete"))
				{
					Planned.Remove(Key);
					continue;
				}
				FSpec Moved = Planned[Key];
				Moved.Location = Parsed.Spec.Location;
				if (Parsed.bHasYaw) Moved.Yaw = Parsed.Spec.Yaw;
				if (!CheckShape(Moved, MaxStepHeight, MaxSlopeDegrees, OutNotes, Error)) return Fail(Error);
				Planned[Key] = Moved;
				if (bWrite)
				{
					const int32 Built = Build(World, Moved, MaxStepHeight);
					if (Built < 0) return Fail(TEXT("the level refused a new actor."));
					OutActorsChanged += Built;
				}
			}
			return true;
		}

		struct FMarker
		{
			FString Name;
			FString Role;
			FVector Location = FVector::ZeroVector;
			/** Where the player would stand: the floor under the marker, once found. */
			FVector Floor = FVector::ZeroVector;
			bool bOnFloor = false;
		};

		TArray<FMarker> ReadMarkers(UWorld& World)
		{
			TArray<FMarker> Markers;
			for (const TPair<FString, TArray<AActor*>>& Group : GroupPieces(World))
			{
				FSpec Spec;
				if (ReadSpec(*Group.Value[0], Spec) && Spec.Shape == TEXT("marker"))
				{
					Markers.Add({Spec.Name, Spec.Role, Group.Value[0]->GetActorLocation()});
				}
			}
			return Markers;
		}

		bool IsTargetRole(const FString& Role)
		{
			return Role == TEXT("goal") || Role == TEXT("objective") || Role == TEXT("checkpoint") || Role == TEXT("pickup");
		}

		UNavigationPath* FindRoute(UWorld& World, const FVector& From, const FVector& To)
		{
			return UNavigationSystemV1::FindPathToLocationSynchronously(&World, From, To);
		}

		FColor RoleColor(const FString& Role)
		{
			if (Role == TEXT("start")) return FColor(80, 220, 100);
			if (Role == TEXT("spawn")) return FColor(240, 80, 70);
			if (Role == TEXT("cover")) return FColor(240, 200, 60);
			if (Role == TEXT("pickup") || Role == TEXT("checkpoint")) return FColor(60, 220, 220);
			return FColor(90, 160, 255);
		}
	}

	UWorld* GetEditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	void DeriveRecommendations(FHyperAIPlayerMetrics& Metrics)
	{
		const float Diameter = Metrics.CapsuleRadius * 2.0f;
		// Doorways about twice the player's width read as passable at speed; corridors fit two players passing.
		Metrics.MinDoorWidth = FMath::Max(Diameter * 2.0f, 120.0f);
		Metrics.MinCorridorWidth = FMath::Max(Diameter * 3.0f, 200.0f);
		Metrics.MinCeilingHeight = Metrics.PlayerHeight * 1.3f;
		Metrics.LowCoverMin = Metrics.CrouchedHeight;
		Metrics.LowCoverMax = FMath::Max(Metrics.LowCoverMin + 20.0f, Metrics.PlayerHeight * 0.6f);
		Metrics.HighCoverMin = Metrics.PlayerHeight + 10.0f;
		Metrics.SafeJumpGap = Metrics.MaxJumpGap * 0.75f;
	}

	bool MeasurePlayer(UWorld& World, const FString& PawnClassPath, FHyperAIPlayerMetrics& OutMetrics, FString& OutError)
	{
		OutMetrics = FHyperAIPlayerMetrics();
		UClass* PawnClass = nullptr;
		if (!PawnClassPath.IsEmpty())
		{
			PawnClass = LoadClass<APawn>(nullptr, *PawnClassPath);
			if (!PawnClass)
			{
				OutError = FString::Printf(TEXT("%s is not a pawn class; pass a class path such as /Game/Characters/B_Hero.B_Hero_C."), *PawnClassPath);
				return false;
			}
		}
		else
		{
			TSubclassOf<AGameModeBase> Mode = World.GetWorldSettings() ? World.GetWorldSettings()->DefaultGameMode : nullptr;
			if (!Mode)
			{
				const FString ModePath = UGameMapsSettings::GetGlobalDefaultGameMode();
				Mode = ModePath.IsEmpty() ? nullptr : LoadClass<AGameModeBase>(nullptr, *ModePath);
			}
			PawnClass = Mode ? Mode.GetDefaultObject()->DefaultPawnClass.Get() : nullptr;
		}
		const ACharacter* Character = PawnClass ? Cast<ACharacter>(PawnClass->GetDefaultObject()) : nullptr;
		OutMetrics.PawnClass = PawnClass ? PawnClass->GetPathName() : FString(TEXT("none"));
		OutMetrics.Source = Character ? TEXT("pawn class defaults")
			: PawnClass ? TEXT("engine Character defaults: the pawn is not a Character")
			: TEXT("engine Character defaults: no default pawn found; pass pawn_class_path");
		if (!Character)
		{
			Character = GetDefault<ACharacter>();
		}
		const UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
		const UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
		const float HalfHeight = Capsule ? Capsule->GetUnscaledCapsuleHalfHeight() : 88.0f;
		OutMetrics.CapsuleRadius = Capsule ? Capsule->GetUnscaledCapsuleRadius() : 34.0f;
		OutMetrics.PlayerHeight = HalfHeight * 2.0f;
		OutMetrics.EyeHeight = HalfHeight + Character->BaseEyeHeight;
		if (Movement)
		{
			OutMetrics.CrouchedHeight = Movement->GetCrouchedHalfHeight() * 2.0f;
			OutMetrics.MaxStepHeight = Movement->MaxStepHeight;
			OutMetrics.MaxWalkableSlopeDegrees = Movement->GetWalkableFloorAngle();
			OutMetrics.WalkSpeed = Movement->MaxWalkSpeed;
			OutMetrics.CrouchSpeed = Movement->MaxWalkSpeedCrouched;
			const float WorldGravity = FMath::Abs(World.GetGravityZ());
			const float Gravity = (WorldGravity > 1.0f ? WorldGravity : 980.0f) * FMath::Max(0.01f, Movement->GravityScale);
			OutMetrics.JumpHeight = FMath::Square(Movement->JumpZVelocity) / (2.0f * Gravity);
			OutMetrics.MaxJumpGap = OutMetrics.WalkSpeed * (2.0f * Movement->JumpZVelocity / Gravity);
		}
		DeriveRecommendations(OutMetrics);
		return true;
	}

	TArray<FHyperAILevelDesignPiece> ReadPieces(UWorld& World)
	{
		TArray<FHyperAILevelDesignPiece> Pieces;
		for (const TPair<FString, TArray<AActor*>>& Group : GroupPieces(World))
		{
			FSpec Spec;
			if (!ReadSpec(*Group.Value[0], Spec)) continue;
			FHyperAILevelDesignPiece& Piece = Pieces.AddDefaulted_GetRef();
			Piece.Name = Spec.Name;
			Piece.Shape = Spec.Shape;
			Piece.Role = Spec.Role;
			Piece.Location = FormatVector(Spec.Location);
			Piece.Size = Spec.Shape == TEXT("marker") ? FString() : FormatVector(Spec.Size);
			Piece.Yaw = static_cast<float>(Spec.Yaw);
			Piece.ActorCount = Group.Value.Num();
		}
		return Pieces;
	}

	FString ComputeRevision(UWorld& World)
	{
		FString Canonical = TEXT("hyperai.level_design.revision.v1\n") + World.GetPathName() + TEXT("\n");
		for (const TPair<FString, TArray<AActor*>>& Group : GroupPieces(World))
		{
			Canonical += Group.Key + TEXT("\n");
			for (const AActor* Actor : Group.Value)
			{
				// Transforms too, so a piece someone dragged in the viewport counts as a change.
				const FTransform Transform = Actor->GetActorTransform();
				Canonical += FString::Printf(TEXT("%s|%s|%s|%s\n"), *Actor->GetActorLabel(), *FormatVector(Transform.GetLocation()),
					*FormatVector(Transform.Rotator().Euler()), *FormatVector(Transform.GetScale3D() * 100.0));
				for (const FName& Tag : Actor->Tags) Canonical += Tag.ToString() + TEXT(";");
				Canonical += TEXT("\n");
			}
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	bool ValidateOps(UWorld& World, const TArray<FHyperAILevelDesignOp>& Ops, const float MaxStepHeight, const float MaxSlopeDegrees,
		TArray<FString>& OutNotes, FString& OutError)
	{
		int32 Unused = 0;
		return RunOps(World, Ops, MaxStepHeight, MaxSlopeDegrees, /*bWrite=*/false, OutNotes, Unused, OutError);
	}

	bool ApplyOps(UWorld& World, const TArray<FHyperAILevelDesignOp>& Ops, const float MaxStepHeight, const float MaxSlopeDegrees,
		int32& OutActorsChanged, FString& OutError)
	{
		TArray<FString> Notes;
		OutActorsChanged = 0;
		if (!ValidateOps(World, Ops, MaxStepHeight, MaxSlopeDegrees, Notes, OutError))
		{
			return false;
		}
		const FScopedTransaction Transaction(LOCTEXT("ApplyBlockoutPlan", "HyperAI: Apply Blockout Plan"));
		Notes.Reset();
		return RunOps(World, Ops, MaxStepHeight, MaxSlopeDegrees, /*bWrite=*/true, Notes, OutActorsChanged, OutError);
	}

	void CheckLayout(UWorld& World, const FHyperAIPlayerMetrics& Metrics, const float MaxSightline, FHyperAILevelDesignValidateReport& Report)
	{
		auto AddIssue = [&Report](const TCHAR* Code, const bool bError, const FString& Subject, const FString& Message, const FVector& Where)
		{
			(bError ? Report.ErrorCount : Report.WarningCount) += 1;
			if (Report.Issues.Num() >= MaxIssues)
			{
				Report.bTruncated = true;
				return;
			}
			Report.Issues.Add({Code, bError ? TEXT("error") : TEXT("warning"), Subject, Message, FormatVector(Where)});
		};
		FCollisionQueryParams Query(SCENE_QUERY_STAT(HyperAILevelDesign), /*bTraceComplex=*/false);
		const float HalfHeight = Metrics.PlayerHeight * 0.5f;

		TArray<FMarker> Markers = ReadMarkers(World);
		for (FMarker& Marker : Markers)
		{
			FHitResult Hit;
			if (!World.LineTraceSingleByChannel(Hit, Marker.Location + FVector(0, 0, 50), Marker.Location - FVector(0, 0, 500), ECC_Visibility, Query))
			{
				AddIssue(TEXT("no_floor"), true, Marker.Name, TEXT("Nothing to stand on within 5 m below this marker."), Marker.Location);
				continue;
			}
			const float Slope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Hit.ImpactNormal.Z, -1.0, 1.0)));
			if (Slope > Metrics.MaxWalkableSlopeDegrees + 0.5f)
			{
				AddIssue(TEXT("steep_floor"), true, Marker.Name, FString::Printf(TEXT("The floor here slopes %.0f degrees; the player slides off above %.0f."),
					Slope, Metrics.MaxWalkableSlopeDegrees), Hit.ImpactPoint);
				continue;
			}
			Marker.Floor = Hit.ImpactPoint;
			Marker.bOnFloor = true;
			const FVector Center = Marker.Floor + FVector(0, 0, HalfHeight + 2.0f);
			if (World.OverlapBlockingTestByChannel(Center, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Metrics.CapsuleRadius, HalfHeight), Query))
			{
				AddIssue(TEXT("no_room"), true, Marker.Name, TEXT("The player's capsule does not fit here; move it away from walls or raise the ceiling."), Marker.Floor);
			}
			FHitResult Ceiling;
			if (World.LineTraceSingleByChannel(Ceiling, Marker.Floor + FVector(0, 0, 5), Marker.Floor + FVector(0, 0, Metrics.MinCeilingHeight), ECC_Visibility, Query))
			{
				AddIssue(TEXT("low_ceiling"), false, Marker.Name, FString::Printf(TEXT("Ceiling %.0f cm above the floor; %.0f or more keeps jumping and cameras comfortable."),
					Ceiling.Distance + 5.0f, Metrics.MinCeilingHeight), Marker.Floor);
			}
			if (Marker.Role == TEXT("cover"))
			{
				// Something within reach at crouched chest height, in any of eight directions, to hide behind.
				bool bCovered = false;
				const FVector Chest = Marker.Floor + FVector(0, 0, Metrics.CrouchedHeight * 0.5f);
				for (int32 Direction = 0; Direction < 8 && !bCovered; ++Direction)
				{
					const FVector Out = FRotator(0.0, Direction * 45.0, 0.0).Vector() * 150.0;
					FHitResult Blocker;
					bCovered = World.LineTraceSingleByChannel(Blocker, Chest, Chest + Out, ECC_Visibility, Query);
				}
				if (!bCovered)
				{
					AddIssue(TEXT("exposed_cover"), false, Marker.Name, TEXT("Nothing within 1.5 m to take cover behind."), Marker.Floor);
				}
			}
		}

		// Cover blocks against the player's heights.
		for (const TPair<FString, TArray<AActor*>>& Group : GroupPieces(World))
		{
			FSpec Spec;
			if (!ReadSpec(*Group.Value[0], Spec)) continue;
			if (Spec.Role == TEXT("cover_low") && (Spec.Size.Z < Metrics.LowCoverMin || Spec.Size.Z > Metrics.LowCoverMax))
			{
				AddIssue(TEXT("cover_height"), false, Spec.Name, FString::Printf(TEXT("Low cover is %.0f cm; %.0f-%.0f hides a crouched player and lets a standing one shoot over."),
					Spec.Size.Z, Metrics.LowCoverMin, Metrics.LowCoverMax), Spec.Location);
			}
			if (Spec.Role == TEXT("cover_high") && Spec.Size.Z < Metrics.HighCoverMin)
			{
				AddIssue(TEXT("cover_height"), false, Spec.Name, FString::Printf(TEXT("High cover is %.0f cm; at least %.0f hides a standing player."),
					Spec.Size.Z, Metrics.HighCoverMin), Spec.Location);
			}
		}

		TArray<const FMarker*> Starts;
		TArray<const FMarker*> Targets;
		TArray<const FMarker*> Spawns;
		for (const FMarker& Marker : Markers)
		{
			if (!Marker.bOnFloor) continue;
			if (Marker.Role == TEXT("start")) Starts.Add(&Marker);
			if (IsTargetRole(Marker.Role)) Targets.Add(&Marker);
			if (Marker.Role == TEXT("spawn")) Spawns.Add(&Marker);
		}
		if (Starts.IsEmpty() && !Targets.IsEmpty())
		{
			AddIssue(TEXT("no_start"), false, FString(), TEXT("There are goals but no start marker, so no route is checked."), Targets[0]->Floor);
		}

		UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(&World);
		const bool bNavData = Navigation && Navigation->GetDefaultNavDataInstance(FNavigationSystem::DontCreate);
		Report.bNavigationReady = bNavData && !Navigation->IsNavigationBuildInProgress();
		if (!Starts.IsEmpty() && !Targets.IsEmpty())
		{
			if (!bNavData)
			{
				AddIssue(TEXT("no_navmesh"), false, FString(), TEXT("The level has no navmesh, so routes were not checked. Add a NavMeshBoundsVolume over the play space."), Starts[0]->Floor);
			}
			else if (!Report.bNavigationReady)
			{
				AddIssue(TEXT("navigation_rebuilding"), false, FString(), TEXT("The navmesh is still rebuilding after the last change; validate again in a moment."), Starts[0]->Floor);
			}
		}
		for (const FMarker* Start : Starts)
		{
			for (const FMarker* Target : Targets)
			{
				if (!bNavData || Report.Routes.Num() >= MaxPairs) break;
				FHyperAILevelDesignRoute& Route = Report.Routes.AddDefaulted_GetRef();
				Route.From = Start->Name;
				Route.To = Target->Name;
				Route.StraightDistance = FVector::Dist(Start->Floor, Target->Floor);
				const UNavigationPath* Path = FindRoute(World, Start->Floor, Target->Floor);
				Route.bReachable = Path && Path->IsValid() && !Path->IsPartial();
				if (!Route.bReachable)
				{
					const FVector End = Path && Path->PathPoints.Num() > 0 ? Path->PathPoints.Last() : Start->Floor;
					AddIssue(TEXT("unreachable"), true, Target->Name, FString::Printf(TEXT("No walkable route from %s; the navmesh path stops here."), *Start->Name), End);
					continue;
				}
				Route.PathLength = Path->GetPathLength();
				// Free width across the route every metre, at waist height, away from both ends.
				float Narrowest = Metrics.MinCorridorWidth * 2.0f;
				FVector NarrowestAt = Start->Floor;
				double Walked = 0.0;
				for (int32 Point = 1; Point < Path->PathPoints.Num(); ++Point)
				{
					const FVector A = Path->PathPoints[Point - 1];
					const FVector B = Path->PathPoints[Point];
					const FVector Along = (B - A).GetSafeNormal2D();
					const FVector Side(-Along.Y, Along.X, 0.0);
					const double Length = FVector::Dist(A, B);
					for (double Offset = 0.0; Offset < Length; Offset += 100.0)
					{
						const double FromStart = Walked + Offset;
						if (FromStart < 150.0 || Route.PathLength - FromStart < 150.0) continue;
						const FVector Waist = A + (B - A) * (Offset / Length) + FVector(0, 0, HalfHeight);
						FHitResult Left;
						FHitResult Right;
						const float Probe = Metrics.MinCorridorWidth;
						const float LeftDistance = World.LineTraceSingleByChannel(Left, Waist, Waist + Side * Probe, ECC_Visibility, Query) ? Left.Distance : Probe;
						const float RightDistance = World.LineTraceSingleByChannel(Right, Waist, Waist - Side * Probe, ECC_Visibility, Query) ? Right.Distance : Probe;
						if (LeftDistance + RightDistance < Narrowest)
						{
							Narrowest = LeftDistance + RightDistance;
							NarrowestAt = Waist - FVector(0, 0, HalfHeight);
						}
					}
					Walked += Length;
				}
				Route.NarrowestWidth = Narrowest;
				if (Narrowest < Metrics.MinDoorWidth)
				{
					AddIssue(TEXT("too_narrow"), true, Route.To, FString::Printf(TEXT("The route from %s squeezes to %.0f cm; doorways need %.0f."),
						*Route.From, Narrowest, Metrics.MinDoorWidth), NarrowestAt);
				}
				else if (Narrowest < Metrics.MinCorridorWidth)
				{
					AddIssue(TEXT("narrow"), false, Route.To, FString::Printf(TEXT("The route from %s narrows to %.0f cm; %.0f lets two players pass."),
						*Route.From, Narrowest, Metrics.MinCorridorWidth), NarrowestAt);
				}
			}
		}

		// Spawns against the places players stand: long open lines are where they get shot from afar.
		for (const FMarker* Spawn : Spawns)
		{
			for (const FMarker& Other : Markers)
			{
				if (!Other.bOnFloor || !(Other.Role == TEXT("start") || IsTargetRole(Other.Role)) || Report.Sightlines.Num() >= MaxPairs) continue;
				FHyperAILevelDesignSightline& Line = Report.Sightlines.AddDefaulted_GetRef();
				Line.From = Spawn->Name;
				Line.To = Other.Name;
				const FVector Eye = Spawn->Floor + FVector(0, 0, Metrics.EyeHeight);
				const FVector Seen = Other.Floor + FVector(0, 0, Metrics.EyeHeight);
				Line.Distance = FVector::Dist(Eye, Seen);
				FHitResult Blocker;
				Line.bVisible = !World.LineTraceSingleByChannel(Blocker, Eye, Seen, ECC_Visibility, Query);
				if (Line.bVisible && Line.Distance > MaxSightline)
				{
					AddIssue(TEXT("long_sightline"), false, Spawn->Name, FString::Printf(TEXT("Open %.0f m sightline to %s; break it with cover or a turn."),
						Line.Distance / 100.0f, *Other.Name), Spawn->Floor);
				}
			}
		}
	}

	FIntPoint ToPixel(const FVector& Point, const FBox& Bounds, const int32 ImageSize)
	{
		// The capture looks straight down with no yaw, so the image's up is +X and its right is +Y.
		const FVector Center = Bounds.GetCenter();
		const double Half = FMath::Max(Bounds.GetExtent().X, Bounds.GetExtent().Y);
		return FIntPoint(
			FMath::FloorToInt((Point.Y - (Center.Y - Half)) / (2.0 * Half) * ImageSize),
			FMath::FloorToInt(((Center.X + Half) - Point.X) / (2.0 * Half) * ImageSize));
	}

	bool WritePreview(UWorld& World, const FHyperAILevelDesignValidateReport& Report, FString& OutPath, FString& OutError)
	{
		FBox Content(ForceInit);
		for (const TPair<FString, TArray<AActor*>>& Group : GroupPieces(World))
		{
			for (const AActor* Actor : Group.Value)
			{
				Content += Actor->GetComponentsBoundingBox(/*bNonColliding=*/true);
				Content += Actor->GetActorLocation();
			}
		}
		if (!Content.IsValid)
		{
			OutError = TEXT("Nothing to frame: place blockout pieces or markers first.");
			return false;
		}
		const double Half = FMath::Max(Content.GetExtent().X, Content.GetExtent().Y) * 1.15 + 300.0;
		const FVector Center = Content.GetCenter();
		const FBox Frame(FVector(Center.X - Half, Center.Y - Half, Content.Min.Z), FVector(Center.X + Half, Center.Y + Half, Content.Max.Z));
		const double CameraZ = Content.Max.Z + 500.0;

		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		Target->RenderTargetFormat = RTF_RGBA32f;
		Target->ClearColor = FLinearColor(1.0e7f, 0, 0, 0);
		Target->InitAutoFormat(PreviewSize, PreviewSize);
		Target->UpdateResourceImmediate(true);
		USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(GetTransientPackage());
		Capture->ProjectionType = ECameraProjectionMode::Orthographic;
		Capture->OrthoWidth = static_cast<float>(Half * 2.0);
		Capture->CaptureSource = SCS_SceneDepth;
		Capture->TextureTarget = Target;
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;
		Capture->SetWorldLocationAndRotation(FVector(Center.X, Center.Y, CameraZ), FRotator(-90.0, 0.0, 0.0));
		Capture->RegisterComponentWithWorld(&World);
		// Pieces placed this frame get their render state at the end of it; the capture must see them now.
		World.SendAllEndOfFrameUpdates();
		Capture->CaptureScene();
		FlushRenderingCommands();
		TArray<FLinearColor> Depth;
		const bool bRead = Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Depth);
		Capture->UnregisterComponent();
		Capture->MarkAsGarbage();
		if (!bRead || Depth.Num() != PreviewSize * PreviewSize)
		{
			OutError = TEXT("The top-down capture could not be read back.");
			return false;
		}

		// Height = camera height minus depth; shade the used range from dark to light.
		const double Far = CameraZ - Frame.Min.Z + 20000.0;
		double Low = TNumericLimits<double>::Max();
		double High = TNumericLimits<double>::Lowest();
		for (const FLinearColor& Sample : Depth)
		{
			if (Sample.R <= 0.0f || Sample.R >= Far) continue;
			Low = FMath::Min(Low, CameraZ - Sample.R);
			High = FMath::Max(High, CameraZ - Sample.R);
		}
		{
			float MinDepth = TNumericLimits<float>::Max();
			float MaxDepth = TNumericLimits<float>::Lowest();
			for (const FLinearColor& Sample : Depth)
			{
				MinDepth = FMath::Min(MinDepth, Sample.R);
				MaxDepth = FMath::Max(MaxDepth, Sample.R);
			}
			UE_LOG(LogHyperAIStudioLevelDesignGate, Verbose, TEXT("Preview depth %f..%f, camera %f, frame z %f..%f, far %f, height %f..%f"),
				MinDepth, MaxDepth, CameraZ, Frame.Min.Z, Frame.Max.Z, Far, Low, High);
		}
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Depth.Num());
		for (int32 Index = 0; Index < Depth.Num(); ++Index)
		{
			const float Sample = Depth[Index].R;
			if (Sample <= 0.0f || Sample >= Far || Low > High)
			{
				Pixels[Index] = FColor(20, 24, 32);
				continue;
			}
			// A flat layout has one height; it still reads as floor rather than as empty space.
			const uint8 Shade = High - Low < 1.0 ? 140 : static_cast<uint8>(50 + 190 * ((CameraZ - Sample) - Low) / (High - Low));
			Pixels[Index] = FColor(Shade, Shade, Shade);
		}

		auto Plot = [&Pixels](const int32 X, const int32 Y, const FColor Colour)
		{
			if (X >= 0 && Y >= 0 && X < PreviewSize && Y < PreviewSize) Pixels[Y * PreviewSize + X] = Colour;
		};
		auto Disc = [&Plot](const FIntPoint At, const int32 Radius, const FColor Colour)
		{
			for (int32 Y = -Radius; Y <= Radius; ++Y)
				for (int32 X = -Radius; X <= Radius; ++X)
					if (X * X + Y * Y <= Radius * Radius) Plot(At.X + X, At.Y + Y, Colour);
		};
		auto Line = [&Disc](const FIntPoint From, const FIntPoint To, const FColor Colour, const bool bDashed)
		{
			const int32 Steps = FMath::Max(FMath::Abs(To.X - From.X), FMath::Abs(To.Y - From.Y));
			for (int32 Step = 0; Step <= Steps; ++Step)
			{
				if (bDashed && (Step / 6) % 2 == 1) continue;
				const float Alpha = Steps > 0 ? static_cast<float>(Step) / Steps : 0.0f;
				Disc(FIntPoint(FMath::RoundToInt(FMath::Lerp<float>(From.X, To.X, Alpha)), FMath::RoundToInt(FMath::Lerp<float>(From.Y, To.Y, Alpha))), 1, Colour);
			}
		};

		const TArray<FMarker> Markers = ReadMarkers(World);
		auto FindMarker = [&Markers](const FString& Name) { return Markers.FindByPredicate([&Name](const FMarker& Marker) { return Marker.Name == Name; }); };
		for (const FHyperAILevelDesignRoute& Route : Report.Routes)
		{
			const FMarker* From = FindMarker(Route.From);
			const FMarker* To = FindMarker(Route.To);
			if (!From || !To) continue;
			const UNavigationPath* Path = Route.bReachable ? FindRoute(World, From->Location, To->Location) : nullptr;
			if (!Path || Path->PathPoints.Num() < 2)
			{
				Line(ToPixel(From->Location, Frame, PreviewSize), ToPixel(To->Location, Frame, PreviewSize), FColor(240, 60, 60), /*bDashed=*/true);
				continue;
			}
			for (int32 Point = 1; Point < Path->PathPoints.Num(); ++Point)
			{
				Line(ToPixel(Path->PathPoints[Point - 1], Frame, PreviewSize), ToPixel(Path->PathPoints[Point], Frame, PreviewSize), FColor(120, 230, 140), false);
			}
		}
		for (const FMarker& Marker : Markers)
		{
			const FIntPoint At = ToPixel(Marker.Location, Frame, PreviewSize);
			Disc(At, 7, FColor(10, 10, 10));
			Disc(At, 5, RoleColor(Marker.Role));
		}
		for (const FHyperAILevelDesignIssue& Issue : Report.Issues)
		{
			FVector Where;
			if (!ParseVector(Issue.Location, Where)) continue;
			const FIntPoint At = ToPixel(Where, Frame, PreviewSize);
			const FColor Colour = Issue.Severity == TEXT("error") ? FColor(255, 40, 40) : FColor(255, 170, 30);
			Line(At - FIntPoint(6, 6), At + FIntPoint(6, 6), Colour, false);
			Line(At + FIntPoint(-6, 6), At + FIntPoint(6, -6), Colour, false);
		}

		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(PreviewSize, PreviewSize, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		OutPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
			/ FString::Printf(TEXT("HyperAIStudio/Previews/level_design_%s.png"), *World.GetMapName()));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutPath), true);
		if (Png.IsEmpty() || !FFileHelper::SaveArrayToFile(Png, *OutPath))
		{
			OutError = TEXT("The preview image could not be written.");
			OutPath.Reset();
			return false;
		}
		return true;
	}
}

#undef LOCTEXT_NAMESPACE

// Games by Hyper 2026.

#include "HyperAIStudioPhysicsToolset.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Engine.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "IO/IoHash.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BoxElem.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/ConvexElem.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/RigidBodyIndexPair.h"
#include "PhysicsEngine/ShapeElem.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/SphereElem.h"
#include "PhysicsEngine/SphylElem.h"
#include "PhysicsEngine/TaperedCapsuleElem.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioPhysics, Log, All);

namespace HyperAIStudio::Physics::Private
{
	constexpr int32 EstimatedBaseReportBytes = 12288;
	constexpr int32 EstimatedBodyBytes = 4096;
	constexpr int32 EstimatedConstraintBytes = 3072;
	constexpr int32 EstimatedPairBytes = 1024;
	constexpr int32 EstimatedIssueBytes = 2048;

	bool IsFinite(const double Value)
	{
		return FMath::IsFinite(Value);
	}

	bool IsFinite(const FVector& Value)
	{
		return IsFinite(Value.X) && IsFinite(Value.Y) && IsFinite(Value.Z);
	}

	bool IsFinite(const FRotator& Value)
	{
		return IsFinite(Value.Pitch) && IsFinite(Value.Yaw) && IsFinite(Value.Roll);
	}

	bool IsFinite(const FQuat& Value)
	{
		return IsFinite(Value.X) && IsFinite(Value.Y) && IsFinite(Value.Z) && IsFinite(Value.W);
	}

	int64 SaturatingAdd(const int64 A, const int64 B)
	{
		return A > MAX_int64 - FMath::Max<int64>(0, B) ? MAX_int64 : A + FMath::Max<int64>(0, B);
	}

	int64 EstimateStringBytes(const FString& Value)
	{
		return SaturatingAdd(64, static_cast<int64>(Value.Len()) * 6);
	}

	int64 EstimateIssueBytes(const FHyperAIPhysicsIssue& Issue)
	{
		int64 Bytes = EstimatedIssueBytes;
		Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Issue.Code));
		Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Issue.Severity));
		Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Issue.StableId));
		Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Issue.Subject));
		return SaturatingAdd(Bytes, EstimateStringBytes(Issue.Detail));
	}

	int64 EstimateBodyRecordBytes(const FHyperAIPhysicsBodyRecord& Record)
	{
		int64 Bytes = EstimatedBodyBytes;
		for (const FString* Value : {&Record.StableId, &Record.BoneName, &Record.BodyObjectPath,
			&Record.BodyFingerprint, &Record.DefaultInstanceFingerprint, &Record.BodySetupGuid,
			&Record.PhysicalMaterialPath, &Record.CollisionProfileName})
		{
			Bytes = SaturatingAdd(Bytes, EstimateStringBytes(*Value));
		}
		return Bytes;
	}

	int64 EstimateConstraintRecordBytes(const FHyperAIPhysicsConstraintRecord& Record)
	{
		int64 Bytes = EstimatedConstraintBytes;
		for (const FString* Value : {&Record.StableId, &Record.TemplateObjectPath,
			&Record.ConstraintFingerprint, &Record.ChildBoneName, &Record.ParentBoneName})
		{
			Bytes = SaturatingAdd(Bytes, EstimateStringBytes(*Value));
		}
		return Bytes;
	}

	int64 EstimatePairRecordBytes(const FHyperAIPhysicsCollisionPairRecord& Record)
	{
		int64 Bytes = EstimatedPairBytes;
		Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Record.StableId));
		Bytes = SaturatingAdd(Bytes, EstimateStringBytes(Record.BodyNameA));
		return SaturatingAdd(Bytes, EstimateStringBytes(Record.BodyNameB));
	}

	void AppendTokenUnchecked(FString& Canonical, const FString& Token)
	{
		Canonical += FString::FromInt(Token.Len());
		Canonical += TEXT(":");
		Canonical += Token;
		Canonical += TEXT("|");
	}

	struct FProjectionContext
	{
		bool bComplete = true;
		int32 WorkUnits = 0;
		int32 Characters = 0;
		TArray<FString> Reasons;

		void MarkIncomplete(const TCHAR* Reason)
		{
			bComplete = false;
			if (Reasons.Num() < 32 && !Reasons.Contains(Reason))
			{
				Reasons.Add(Reason);
			}
		}

		bool ConsumeWork(const int32 Amount, const TCHAR* Reason)
		{
			if (Amount < 0 || WorkUnits > FHyperAIStudioPhysicsContracts::MaxProjectionContainerElements - Amount)
			{
				MarkIncomplete(Reason);
				return false;
			}
			WorkUnits += Amount;
			return true;
		}

		bool Append(FString& Canonical, const FString& Token, const TCHAR* Reason)
		{
			if (Token.Len() > FHyperAIStudioPhysicsContracts::MaxPathCharacters
				|| Characters > FHyperAIStudioPhysicsContracts::MaxProjectionCharacters
					- Token.Len() - 32)
			{
				MarkIncomplete(Reason);
				return false;
			}
			Characters += Token.Len() + 32;
			AppendTokenUnchecked(Canonical, Token);
			return true;
		}
	};

	FString HashCanonical(const FString& Canonical)
	{
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	FString CanonicalFloat(const double Value)
	{
		return IsFinite(Value) ? FString::Printf(TEXT("%.17g"), Value) : FString();
	}

	void AppendFloat(FProjectionContext& Context, FString& Canonical, const double Value,
		const TCHAR* Reason)
	{
		const FString Token = CanonicalFloat(Value);
		if (Token.IsEmpty())
		{
			Context.MarkIncomplete(Reason);
			return;
		}
		Context.Append(Canonical, Token, Reason);
	}

	void AppendVector(FProjectionContext& Context, FString& Canonical, const FVector& Value,
		const TCHAR* Reason)
	{
		AppendFloat(Context, Canonical, Value.X, Reason);
		AppendFloat(Context, Canonical, Value.Y, Reason);
		AppendFloat(Context, Canonical, Value.Z, Reason);
	}

	void AppendRotator(FProjectionContext& Context, FString& Canonical, const FRotator& Value,
		const TCHAR* Reason)
	{
		AppendFloat(Context, Canonical, Value.Pitch, Reason);
		AppendFloat(Context, Canonical, Value.Yaw, Reason);
		AppendFloat(Context, Canonical, Value.Roll, Reason);
	}

	void AppendTransform(FProjectionContext& Context, FString& Canonical,
		const FTransform& Value, const TCHAR* Reason)
	{
		if (!Value.IsValid() || !IsFinite(Value.GetTranslation()) || !IsFinite(Value.GetRotation())
			|| !IsFinite(Value.GetScale3D()))
		{
			Context.MarkIncomplete(Reason);
			return;
		}
		AppendVector(Context, Canonical, Value.GetTranslation(), Reason);
		AppendFloat(Context, Canonical, Value.GetRotation().X, Reason);
		AppendFloat(Context, Canonical, Value.GetRotation().Y, Reason);
		AppendFloat(Context, Canonical, Value.GetRotation().Z, Reason);
		AppendFloat(Context, Canonical, Value.GetRotation().W, Reason);
		AppendVector(Context, Canonical, Value.GetScale3D(), Reason);
	}

	void AddIssue(TArray<FHyperAIPhysicsIssue>& Issues, const FString& Code,
		const FString& Severity, const FString& Subject, const FString& Detail)
	{
		if (Issues.Num() >= FHyperAIStudioPhysicsContracts::MaxIssues)
		{
			return;
		}
		FHyperAIPhysicsIssue Issue;
		Issue.Code = Code.Left(96);
		Issue.Severity = Severity.Left(16);
		Issue.Subject = Subject.Left(FHyperAIStudioPhysicsContracts::MaxPathCharacters);
		Issue.Detail = Detail.Left(1024);
		FString Canonical;
		AppendTokenUnchecked(Canonical, TEXT("hyperai.physics.issue.v1"));
		AppendTokenUnchecked(Canonical, Issue.Code);
		AppendTokenUnchecked(Canonical, Issue.Subject);
		AppendTokenUnchecked(Canonical, Issue.Detail);
		Issue.StableId = HashCanonical(Canonical);
		Issues.Add(MoveTemp(Issue));
	}

	bool IsSafeName(const FString& Value, const bool bAllowNone = false)
	{
		if (Value.IsEmpty() || Value.Len() > FHyperAIStudioPhysicsContracts::MaxNameCharacters
			|| (!bAllowNone && Value.Equals(TEXT("None"), ESearchCase::IgnoreCase)))
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!(FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('-')))
			{
				return false;
			}
		}
		return true;
	}

	bool AppendShapeBase(const FKShapeElem& Shape, FProjectionContext& Context,
		FString& Canonical)
	{
		const FString Name = Shape.GetName().ToString();
		if (Name.Len() > FHyperAIStudioPhysicsContracts::MaxNameCharacters)
		{
			Context.MarkIncomplete(TEXT("shape_name_too_long"));
			return false;
		}
		Context.Append(Canonical, Name, TEXT("shape_name_projection_exceeded"));
		Context.Append(Canonical, FString::FromInt(static_cast<int32>(Shape.GetShapeType())),
			TEXT("shape_type_projection_exceeded"));
		AppendFloat(Context, Canonical, Shape.RestOffset, TEXT("shape_rest_offset_invalid"));
		Context.Append(Canonical, Shape.GetContributeToMass() ? TEXT("mass") : TEXT("no_mass"),
			TEXT("shape_mass_projection_exceeded"));
		Context.Append(Canonical,
			FString::FromInt(static_cast<int32>(Shape.GetCollisionEnabled())),
			TEXT("shape_collision_projection_exceeded"));
#if WITH_EDITORONLY_DATA
		Context.Append(Canonical, Shape.bIsGenerated ? TEXT("generated") : TEXT("authored"),
			TEXT("shape_generation_projection_exceeded"));
#endif
		return Context.bComplete;
	}

	template <typename ShapeType, typename Projection>
	void ProjectShapeArray(const TArray<ShapeType>& Shapes, const TCHAR* Kind,
		FProjectionContext& Context, FString& Canonical, int32& InvalidCount,
		Projection&& Project)
	{
		if (Shapes.Num() > FHyperAIStudioPhysicsContracts::MaxShapesPerBody
			|| !Context.ConsumeWork(Shapes.Num(), TEXT("shape_projection_work_exceeded")))
		{
			Context.MarkIncomplete(TEXT("shape_count_not_bounded_before_projection"));
			return;
		}
		Context.Append(Canonical, Kind, TEXT("shape_kind_projection_exceeded"));
		Context.Append(Canonical, FString::FromInt(Shapes.Num()),
			TEXT("shape_count_projection_exceeded"));
		for (const ShapeType& Shape : Shapes)
		{
			FString ShapeCanonical;
			AppendShapeBase(Shape, Context, ShapeCanonical);
			if (!Project(Shape, Context, ShapeCanonical))
			{
				++InvalidCount;
			}
			Context.Append(Canonical, HashCanonical(ShapeCanonical),
				TEXT("shape_hash_projection_exceeded"));
		}
	}

	bool BuildShapesFingerprint(const FKAggregateGeom& Geometry, FProjectionContext& Context,
		FString& OutFingerprint, int32& OutInvalidCount, int32& OutUnsupportedCount)
	{
		OutFingerprint.Reset();
		OutInvalidCount = 0;
		OutUnsupportedCount = Geometry.LevelSetElems.Num() + Geometry.SkinnedLevelSetElems.Num()
			+ Geometry.MLLevelSetElems.Num() + Geometry.SkinnedTriangleMeshElems.Num();
		const int64 Total = static_cast<int64>(Geometry.SphereElems.Num())
			+ Geometry.BoxElems.Num() + Geometry.SphylElems.Num() + Geometry.ConvexElems.Num()
			+ Geometry.TaperedCapsuleElems.Num() + OutUnsupportedCount;
		if (Total < 0 || Total > FHyperAIStudioPhysicsContracts::MaxShapesPerBody)
		{
			Context.MarkIncomplete(TEXT("body_shape_count_not_bounded_before_projection"));
			return false;
		}
		if (OutUnsupportedCount > 0)
		{
			Context.MarkIncomplete(TEXT("levelset_or_skinned_shape_projection_unsupported"));
		}

		FString Canonical;
		Context.Append(Canonical, TEXT("hyperai.physics.shapes.v1"),
			TEXT("shape_projection_exceeded"));
		ProjectShapeArray(Geometry.SphereElems, TEXT("sphere"), Context, Canonical,
			OutInvalidCount, [](const FKSphereElem& Shape, FProjectionContext& Local, FString& Value)
			{
				AppendVector(Local, Value, Shape.Center, TEXT("sphere_center_invalid"));
				AppendFloat(Local, Value, Shape.Radius, TEXT("sphere_radius_invalid"));
				return IsFinite(Shape.Center) && IsFinite(Shape.Radius) && Shape.Radius > 0.0;
			});
		ProjectShapeArray(Geometry.BoxElems, TEXT("box"), Context, Canonical,
			OutInvalidCount, [](const FKBoxElem& Shape, FProjectionContext& Local, FString& Value)
			{
				AppendVector(Local, Value, Shape.Center, TEXT("box_center_invalid"));
				AppendRotator(Local, Value, Shape.Rotation, TEXT("box_rotation_invalid"));
				AppendFloat(Local, Value, Shape.X, TEXT("box_x_invalid"));
				AppendFloat(Local, Value, Shape.Y, TEXT("box_y_invalid"));
				AppendFloat(Local, Value, Shape.Z, TEXT("box_z_invalid"));
				return IsFinite(Shape.Center) && IsFinite(Shape.Rotation) && IsFinite(Shape.X)
					&& IsFinite(Shape.Y) && IsFinite(Shape.Z)
					&& Shape.X > 0.0 && Shape.Y > 0.0 && Shape.Z > 0.0;
			});
		ProjectShapeArray(Geometry.SphylElems, TEXT("capsule"), Context, Canonical,
			OutInvalidCount, [](const FKSphylElem& Shape, FProjectionContext& Local, FString& Value)
			{
				AppendVector(Local, Value, Shape.Center, TEXT("capsule_center_invalid"));
				AppendRotator(Local, Value, Shape.Rotation, TEXT("capsule_rotation_invalid"));
				AppendFloat(Local, Value, Shape.Radius, TEXT("capsule_radius_invalid"));
				AppendFloat(Local, Value, Shape.Length, TEXT("capsule_length_invalid"));
				return IsFinite(Shape.Center) && IsFinite(Shape.Rotation)
					&& IsFinite(Shape.Radius) && IsFinite(Shape.Length)
					&& Shape.Radius > 0.0 && Shape.Length >= 0.0;
			});
		ProjectShapeArray(Geometry.TaperedCapsuleElems, TEXT("tapered_capsule"),
			Context, Canonical, OutInvalidCount,
			[](const FKTaperedCapsuleElem& Shape, FProjectionContext& Local, FString& Value)
			{
				AppendVector(Local, Value, Shape.Center, TEXT("tapered_center_invalid"));
				AppendRotator(Local, Value, Shape.Rotation, TEXT("tapered_rotation_invalid"));
				AppendFloat(Local, Value, Shape.Radius0, TEXT("tapered_radius0_invalid"));
				AppendFloat(Local, Value, Shape.Radius1, TEXT("tapered_radius1_invalid"));
				AppendFloat(Local, Value, Shape.Length, TEXT("tapered_length_invalid"));
				AppendFloat(Local, Value, Shape.Width, TEXT("tapered_width_invalid"));
				Local.Append(Value, Shape.bOneSidedCollision ? TEXT("one_sided") : TEXT("two_sided"),
					TEXT("tapered_side_projection_exceeded"));
				return IsFinite(Shape.Center) && IsFinite(Shape.Rotation)
					&& IsFinite(Shape.Radius0) && IsFinite(Shape.Radius1)
					&& IsFinite(Shape.Length) && IsFinite(Shape.Width)
					&& Shape.Radius0 > 0.0 && Shape.Radius1 > 0.0
					&& Shape.Length >= 0.0 && Shape.Width >= 0.0;
			});
		ProjectShapeArray(Geometry.ConvexElems, TEXT("convex"), Context, Canonical,
			OutInvalidCount, [](const FKConvexElem& Shape, FProjectionContext& Local, FString& Value)
			{
				const int32 VertexCount = Shape.VertexData.Num();
				const int32 IndexCount = Shape.IndexData.Num();
				if (VertexCount > FHyperAIStudioPhysicsContracts::MaxConvexVerticesPerShape
					|| IndexCount > FHyperAIStudioPhysicsContracts::MaxConvexIndicesPerShape
					|| !Local.ConsumeWork(VertexCount + IndexCount,
						TEXT("convex_projection_work_exceeded")))
				{
					Local.MarkIncomplete(TEXT("convex_container_not_bounded_before_projection"));
					return false;
				}
				Local.Append(Value, FString::FromInt(VertexCount),
					TEXT("convex_vertex_count_projection_exceeded"));
				for (const FVector& Vertex : Shape.VertexData)
				{
					AppendVector(Local, Value, Vertex, TEXT("convex_vertex_invalid"));
				}
				Local.Append(Value, FString::FromInt(IndexCount),
					TEXT("convex_index_count_projection_exceeded"));
				bool bIndicesValid = IndexCount % 3 == 0;
				for (const int32 Index : Shape.IndexData)
				{
					Local.Append(Value, FString::FromInt(Index),
						TEXT("convex_index_projection_exceeded"));
					bIndicesValid &= Index >= 0 && Index < VertexCount;
				}
				AppendVector(Local, Value, Shape.ElemBox.Min, TEXT("convex_box_min_invalid"));
				AppendVector(Local, Value, Shape.ElemBox.Max, TEXT("convex_box_max_invalid"));
				AppendTransform(Local, Value, Shape.GetTransform(), TEXT("convex_transform_invalid"));
				return VertexCount >= 4 && IndexCount >= 3 && bIndicesValid
					&& Shape.VertexData.ContainsByPredicate([](const FVector& Vertex)
					{
						return !IsFinite(Vertex);
					}) == false;
			});
		OutFingerprint = HashCanonical(Canonical);
		if (!FHyperAIStudioPhysicsContracts::IsCanonicalSha256(OutFingerprint))
		{
			Context.MarkIncomplete(TEXT("shape_fingerprint_failed"));
			return false;
		}
		return Context.bComplete && OutInvalidCount == 0 && OutUnsupportedCount == 0;
	}

	FString BuildDefaultInstanceFingerprint(const FBodyInstance& Instance,
		FProjectionContext& Context)
	{
		FString Canonical;
		const FString CollisionProfileName = Instance.GetCollisionProfileName().ToString();
		if (CollisionProfileName.Len() > FHyperAIStudioPhysicsContracts::MaxNameCharacters)
		{
			Context.MarkIncomplete(TEXT("collision_profile_name_too_long"));
		}
		Context.Append(Canonical, TEXT("hyperai.physics.body-instance.v1"),
			TEXT("body_instance_projection_exceeded"));
		Context.Append(Canonical, CollisionProfileName,
			TEXT("collision_profile_projection_exceeded"));
		Context.Append(Canonical,
			FString::FromInt(static_cast<int32>(Instance.GetCollisionEnabled(false))),
			TEXT("collision_enabled_projection_exceeded"));
		Context.Append(Canonical, Instance.bSimulatePhysics ? TEXT("simulate") : TEXT("not_simulate"),
			TEXT("simulate_projection_exceeded"));
		Context.Append(Canonical, Instance.bEnableGravity ? TEXT("gravity") : TEXT("no_gravity"),
			TEXT("gravity_projection_exceeded"));
		Context.Append(Canonical, Instance.bStartAwake ? TEXT("awake") : TEXT("sleep"),
			TEXT("awake_projection_exceeded"));
		Context.Append(Canonical, Instance.bUseCCD ? TEXT("ccd") : TEXT("no_ccd"),
			TEXT("ccd_projection_exceeded"));
		Context.Append(Canonical, FString::FromInt(static_cast<int32>(Instance.GetObjectType())),
			TEXT("object_type_projection_exceeded"));
		AppendFloat(Context, Canonical, Instance.MassScale, TEXT("mass_scale_invalid"));
		AppendFloat(Context, Canonical, Instance.LinearDamping, TEXT("linear_damping_invalid"));
		AppendFloat(Context, Canonical, Instance.AngularDamping, TEXT("angular_damping_invalid"));
		AppendVector(Context, Canonical, Instance.COMNudge, TEXT("com_nudge_invalid"));
		AppendVector(Context, Canonical, Instance.InertiaTensorScale, TEXT("inertia_scale_invalid"));
		return HashCanonical(Canonical);
	}

	FString BuildConstraintFingerprint(const FConstraintInstance& Instance,
		FProjectionContext& Context)
	{
		FString Canonical;
		Context.Append(Canonical, TEXT("hyperai.physics.constraint.v1"),
			TEXT("constraint_projection_exceeded"));
		Context.Append(Canonical, Instance.JointName.ToString(),
			TEXT("constraint_joint_name_projection_exceeded"));
		Context.Append(Canonical, Instance.GetChildBoneName().ToString(),
			TEXT("constraint_child_projection_exceeded"));
		Context.Append(Canonical, Instance.GetParentBoneName().ToString(),
			TEXT("constraint_parent_projection_exceeded"));
		Context.Append(Canonical, FString::FromInt(static_cast<int32>(Instance.GetLinearXMotion())),
			TEXT("constraint_linear_x_projection_exceeded"));
		Context.Append(Canonical, FString::FromInt(static_cast<int32>(Instance.GetLinearYMotion())),
			TEXT("constraint_linear_y_projection_exceeded"));
		Context.Append(Canonical, FString::FromInt(static_cast<int32>(Instance.GetLinearZMotion())),
			TEXT("constraint_linear_z_projection_exceeded"));
		Context.Append(Canonical, FString::FromInt(static_cast<int32>(Instance.GetAngularSwing1Motion())),
			TEXT("constraint_swing1_projection_exceeded"));
		Context.Append(Canonical, FString::FromInt(static_cast<int32>(Instance.GetAngularSwing2Motion())),
			TEXT("constraint_swing2_projection_exceeded"));
		Context.Append(Canonical, FString::FromInt(static_cast<int32>(Instance.GetAngularTwistMotion())),
			TEXT("constraint_twist_projection_exceeded"));
		AppendFloat(Context, Canonical, Instance.GetLinearLimit(),
			TEXT("constraint_linear_limit_invalid"));
		AppendFloat(Context, Canonical, Instance.GetAngularSwing1Limit(),
			TEXT("constraint_swing1_limit_invalid"));
		AppendFloat(Context, Canonical, Instance.GetAngularSwing2Limit(),
			TEXT("constraint_swing2_limit_invalid"));
		AppendFloat(Context, Canonical, Instance.GetAngularTwistLimit(),
			TEXT("constraint_twist_limit_invalid"));
		Context.Append(Canonical, Instance.ProfileInstance.bDisableCollision
			? TEXT("collision_disabled") : TEXT("collision_enabled"),
			TEXT("constraint_collision_projection_exceeded"));
		Context.Append(Canonical, Instance.ProfileInstance.bEnableProjection
			? TEXT("projection_enabled") : TEXT("projection_disabled"),
			TEXT("constraint_projection_flag_exceeded"));
		Context.Append(Canonical, Instance.ProfileInstance.bParentDominates
			? TEXT("parent_dominates") : TEXT("no_parent_dominance"),
			TEXT("constraint_parent_flag_exceeded"));
		AppendTransform(Context, Canonical, Instance.GetRefFrame(EConstraintFrame::Frame1),
			TEXT("constraint_frame1_invalid"));
		AppendTransform(Context, Canonical, Instance.GetRefFrame(EConstraintFrame::Frame2),
			TEXT("constraint_frame2_invalid"));
		return HashCanonical(Canonical);
	}

	FString BuildSolverFingerprint(const FHyperAIPhysicsSolverRecord& Solver)
	{
		FString Canonical;
		AppendTokenUnchecked(Canonical, TEXT("hyperai.physics.solver.v1"));
		AppendTokenUnchecked(Canonical, FString::FromInt(Solver.PositionIterations));
		AppendTokenUnchecked(Canonical, FString::FromInt(Solver.VelocityIterations));
		AppendTokenUnchecked(Canonical, FString::FromInt(Solver.ProjectionIterations));
		AppendTokenUnchecked(Canonical, CanonicalFloat(Solver.CullDistance));
		AppendTokenUnchecked(Canonical, CanonicalFloat(Solver.MaxDepenetrationVelocity));
		AppendTokenUnchecked(Canonical, CanonicalFloat(Solver.FixedTimeStep));
		AppendTokenUnchecked(Canonical, Solver.bUseLinearJointSolver ? TEXT("linear") : TEXT("nonlinear"));
		AppendTokenUnchecked(Canonical, Solver.bUseManifolds ? TEXT("manifolds") : TEXT("single_contact"));
		return HashCanonical(Canonical);
	}

	bool DeadlineExceeded(const double DeadlineSeconds)
	{
		return FPlatformTime::Seconds() > DeadlineSeconds;
	}

	FString PhysicsObjectPath(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString();
	}
}

FString FHyperAIStudioPhysicsContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioPhysics.HyperAIStudioPhysicsToolset");
}

const TArray<FHyperAIStudioPhysicsManifestEntry>& FHyperAIStudioPhysicsContracts::GetManifest()
{
	static const TArray<FHyperAIStudioPhysicsManifestEntry> Manifest = {
		{TEXT("hyper_physics_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_physics_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_physics_validate"), GetQualifiedToolsetName()}};
	return Manifest;
}

bool FHyperAIStudioPhysicsContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	const TArray<FHyperAIStudioPhysicsManifestEntry>& Manifest = GetManifest();
	if (Manifest.Num() != 3)
	{
		return false;
	}
	TSet<FString> Unique;
	TArray<FString> Names;
	for (const FHyperAIStudioPhysicsManifestEntry& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| Unique.Contains(Entry.Name))
		{
			return false;
		}
		Unique.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

const TArray<FString>& FHyperAIStudioPhysicsContracts::GetEpicDelegates()
{
	static const TArray<FString> Delegates = {
		TEXT("ChaosClothAssetToolset.ChaosClothAssetToolset.AssignClothingToSection"),
		TEXT("ChaosClothAssetToolset.ChaosClothAssetToolset.ConvertClothingAssetCommonToChaosClothAsset"),
		TEXT("ChaosClothAssetToolset.ChaosClothAssetToolset.CreateClothingAsset"),
		TEXT("ChaosClothAssetToolset.ChaosClothAssetToolset.GetSectionClothing"),
		TEXT("ChaosClothAssetToolset.ChaosClothAssetToolset.ListClothingAssets"),
		TEXT("ChaosClothAssetToolset.ChaosClothAssetToolset.RemoveClothingFromSection"),
		TEXT("PhysicsToolsets.PhysicsAssetToolset.AddBody"),
		TEXT("PhysicsToolsets.PhysicsAssetToolset.AddConstraint"),
		TEXT("PhysicsToolsets.PhysicsAssetToolset.CreateFromMesh"),
		TEXT("PhysicsToolsets.PhysicsAssetToolset.GetBodyMassScale"),
		TEXT("PhysicsToolsets.PhysicsAssetToolset.GetBodyNames"),
		TEXT("PhysicsToolsets.PhysicsAssetToolset.GetBodyPhysicsMode"),
		TEXT("PhysicsToolsets.PhysicsAssetToolset.GetBodyShapes"),
		TEXT("PhysicsToolsets.PhysicsAssetToolset.GetConstraints"),
		TEXT("PhysicsToolsets.PhysicsAssetToolset.RemoveBody"),
		TEXT("PhysicsToolsets.PhysicsAssetToolset.RemoveConstraint"),
		TEXT("PhysicsToolsets.PhysicsAssetToolset.RemoveShape"),
		TEXT("PhysicsToolsets.PhysicsAssetToolset.SetBodyMassScale"),
		TEXT("PhysicsToolsets.PhysicsAssetToolset.SetBodyPhysicsMode"),
		TEXT("PhysicsToolsets.PhysicsAssetToolset.SetBox"),
		TEXT("PhysicsToolsets.PhysicsAssetToolset.SetCapsule"),
		TEXT("PhysicsToolsets.PhysicsAssetToolset.SetConstraintLimits"),
		TEXT("PhysicsToolsets.PhysicsAssetToolset.SetSphere")};
	return Delegates;
}

const TArray<FString>& FHyperAIStudioPhysicsContracts::GetPythonDelegates()
{
	static const TArray<FString> Delegates = {
		TEXT("EditorToolset.SceneTools.get_collision_channels"),
		TEXT("EditorToolset.SkeletalMeshTools.assign_physics_asset"),
		TEXT("EditorToolset.SkeletalMeshTools.get_physics_asset"),
		TEXT("EditorToolset.StaticMeshTools.generate_convex_collisions"),
		TEXT("EditorToolset.StaticMeshTools.remove_collisions")};
	return Delegates;
}

TArray<FHyperAIPhysicsCapabilityStatus> FHyperAIStudioPhysicsContracts::GetCapabilityMatrix()
{
	FHyperAIPhysicsCapabilityStatus Physics;
	Physics.DelegatedEpicCallables = GetEpicDelegates();
	Physics.DelegatedPythonCallables = GetPythonDelegates();
	Physics.ClosedCases = {
		TEXT("loaded-only exact UPhysicsAsset structural snapshot"),
		TEXT("clean persisted CAS with fail-closed Asset Registry tri-state"),
		TEXT("collision-profile, collision-enabled, simulate, solver, and constraint-collision preflight"),
		TEXT("detached structural value validation")};
	Physics.UnsupportedCases = {
		TEXT("Epic-equivalent body/shape/constraint-limit/cloth operations"),
		TEXT("level-set, ML level-set, skinned level-set, and skinned triangle serialization"),
		TEXT("compile, simulation, physics-state rebuild, save, or fresh post-effect proof")};
	Physics.Remediation = TEXT("Use Epic's exact Physics/Cloth callables for covered work. Concrete HyperAI edits require a hard-bounded compile/simulation/save continuation backend.");
	return {MoveTemp(Physics)};
}

bool FHyperAIStudioPhysicsContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || !Path.StartsWith(TEXT("/Game/"))
		|| Path.Contains(TEXT("*")) || Path.Contains(TEXT("?")) || Path.Contains(TEXT(":")))
	{
		return false;
	}
	for (const TCHAR Character : Path)
	{
		if (FChar::IsControl(Character))
		{
			return false;
		}
	}
	FText Reason;
	if (!FPackageName::IsValidObjectPath(Path, &Reason))
	{
		return false;
	}
	const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
	const FString ObjectName = FPackageName::ObjectPathToObjectName(Path);
	return !PackageName.IsEmpty() && !ObjectName.IsEmpty()
		&& FPackageName::GetLongPackageAssetName(PackageName) == ObjectName;
}

bool FHyperAIStudioPhysicsContracts::IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:")))
	{
		return false;
	}
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f'))))
		{
			return false;
		}
	}
	return true;
}

FString FHyperAIStudioPhysicsContracts::ClassifyAssetRegistryExistence(
	const UE::AssetRegistry::EExists State)
{
	switch (State)
	{
	case UE::AssetRegistry::EExists::Exists:
		return TEXT("exists");
	case UE::AssetRegistry::EExists::DoesNotExist:
		return TEXT("does_not_exist");
	default:
		return TEXT("unknown");
	}
}

namespace HyperAIStudio::Physics::Private
{
	FString CursorSeal(const FString& TargetPath, const FString& Revision,
		const int32 PageSize)
	{
		FString Canonical;
		AppendTokenUnchecked(Canonical, TEXT("hyperai.physics.inspect-cursor.v1"));
		AppendTokenUnchecked(Canonical, TargetPath);
		AppendTokenUnchecked(Canonical, Revision);
		AppendTokenUnchecked(Canonical, FString::FromInt(PageSize));
		return HashCanonical(Canonical);
	}
}

FString FHyperAIStudioPhysicsContracts::BuildCursor(const FString& TargetPath,
	const FString& Revision, const int32 PageSize, const int32 Offset)
{
	if (!IsCanonicalProjectObjectPath(TargetPath) || !IsCanonicalSha256(Revision)
		|| PageSize < 1 || PageSize > MaxPageSize || Offset < 0
		|| Offset > MaxBodies + MaxConstraints + MaxDisabledCollisionPairs)
	{
		return {};
	}
	return HyperAIStudio::Physics::Private::CursorSeal(TargetPath, Revision, PageSize)
		+ TEXT(".") + FString::FromInt(Offset);
}

bool FHyperAIStudioPhysicsContracts::ParseCursor(const FString& Cursor,
	const FString& TargetPath, const FString& Revision, const int32 PageSize,
	int32& OutOffset)
{
	OutOffset = 0;
	if (Cursor.Len() > MaxCursorCharacters || !IsCanonicalProjectObjectPath(TargetPath)
		|| !IsCanonicalSha256(Revision) || PageSize < 1 || PageSize > MaxPageSize)
	{
		return false;
	}
	if (Cursor.IsEmpty())
	{
		return true;
	}
	int32 Separator = INDEX_NONE;
	if (!Cursor.FindLastChar(TEXT('.'), Separator) || Separator <= 0
		|| Separator >= Cursor.Len() - 1
		|| Cursor.Left(Separator) != HyperAIStudio::Physics::Private::CursorSeal(
			TargetPath, Revision, PageSize))
	{
		return false;
	}
	const FString OffsetText = Cursor.Mid(Separator + 1);
	int64 Offset = 0;
	for (const TCHAR Character : OffsetText)
	{
		if (Character < TEXT('0') || Character > TEXT('9'))
		{
			return false;
		}
		Offset = Offset * 10 + Character - TEXT('0');
		if (Offset > MaxBodies + MaxConstraints + MaxDisabledCollisionPairs)
		{
			return false;
		}
	}
	OutOffset = static_cast<int32>(Offset);
	return true;
}

FString FHyperAIStudioPhysicsContracts::InspectPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("physics.inspect.v1|target:string|page:int|cursor:string|max_game_thread_ms:int|max_output_bytes:int"));
	return Value;
}

FString FHyperAIStudioPhysicsContracts::PatchPayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("physics.settings-patch.v1|target:string|base:sha256|closed_patch:typed|semantic:sha256"));
	return Value;
}

FString FHyperAIStudioPhysicsContracts::ValidatePayloadSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("physics.validate.v1|target:string|expected:sha256?|policy:enum|max_issues:int|max_game_thread_ms:int|max_output_bytes:int"));
	return Value;
}

FString FHyperAIStudioPhysicsContracts::InspectResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("physics.inspect-result.v1|asset:record|bodies:bounded|constraints:bounded|pairs:bounded|issues:bounded"));
	return Value;
}

FString FHyperAIStudioPhysicsContracts::ValidateResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("physics.validate-result.v1|valid:bool|complete:bool|validator:sha256|issues:bounded"));
	return Value;
}

FString FHyperAIStudioPhysicsContracts::MutationResultSchemaFingerprint()
{
	static const FString Value = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("physics.mutation-blocked-result.v1|terminal:false|effect_count:zero"));
	return Value;
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioPhysicsContracts::GetAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.physics.loaded_exact.ue58");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		// Both generated physics groups are blocking. Only core owns their authority;
		// optional adapters may enumerate exact generated non-blocking groups here.
		Value.ApplicableNonBlockingRequirementGroupIds.Reset();
		Value.Variants.Add({TEXT("hyper_physics_inspect"), InspectVariantId,
			InspectPayloadTypeId, InspectPayloadSchemaFingerprint(), InspectResultTypeId,
			InspectResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({TEXT("hyper_physics_apply_plan"), MutationVariantId,
			PatchPayloadTypeId, PatchPayloadSchemaFingerprint(), MutationResultTypeId,
			MutationResultSchemaFingerprint(),
			EHyperAIStudioDomainSafety::Edit});
		Value.Variants.Add({TEXT("hyper_physics_validate"), ValidateVariantId,
			ValidatePayloadTypeId, ValidatePayloadSchemaFingerprint(), ValidateResultTypeId,
			ValidateResultSchemaFingerprint(), EHyperAIStudioDomainSafety::Read});
		Value.ContractFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

FString FHyperAIStudioPhysicsInspectPayload::GetTypeId() const
{
	return FHyperAIStudioPhysicsContracts::InspectPayloadTypeId;
}

FString FHyperAIStudioPhysicsInspectPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPhysicsContracts::InspectPayloadSchemaFingerprint();
}

int32 FHyperAIStudioPhysicsInspectPayload::GetBoundedByteSize() const
{
	return FMath::Min<int64>(MAX_int32, 128ll + 2ll * (Request.TargetPath.Len()
		+ Request.Cursor.Len()));
}

FString FHyperAIStudioPhysicsValidatePayload::GetTypeId() const
{
	return FHyperAIStudioPhysicsContracts::ValidatePayloadTypeId;
}

FString FHyperAIStudioPhysicsValidatePayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPhysicsContracts::ValidatePayloadSchemaFingerprint();
}

int32 FHyperAIStudioPhysicsValidatePayload::GetBoundedByteSize() const
{
	return FMath::Min<int64>(MAX_int32, 160ll + 2ll * (Request.TargetPath.Len()
		+ Request.ExpectedPersistedRevision.Len() + Request.Policy.Len()));
}

FString FHyperAIStudioPhysicsPatchPayload::GetTypeId() const
{
	return FHyperAIStudioPhysicsContracts::PatchPayloadTypeId;
}

FString FHyperAIStudioPhysicsPatchPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPhysicsContracts::PatchPayloadSchemaFingerprint();
}

int32 FHyperAIStudioPhysicsPatchPayload::GetBoundedByteSize() const
{
	return FMath::Min<int64>(MAX_int32, 512ll + 2ll * (TargetPath.Len()
		+ BasePersistedRevision.Len() + Patch.BodyName.Len()
		+ Patch.ExpectedBodyFingerprint.Len() + Patch.CollisionProfileName.Len()
		+ Patch.ExpectedSolverFingerprint.Len() + Patch.ConstraintStableId.Len()
		+ Patch.ExpectedConstraintFingerprint.Len() + SemanticFingerprint.Len()));
}

FString FHyperAIStudioPhysicsPatchPayload::GetSemanticFingerprint() const
{
	return SemanticFingerprint;
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioPhysicsPatchPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioPhysicsPatchPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioPhysicsPatchPayload, ESPMode::ThreadSafe>();
	Clone->TargetPath = TargetPath;
	Clone->BasePersistedRevision = BasePersistedRevision;
	Clone->Patch = Patch;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioPhysicsInspectResultPayload::GetTypeId() const
{
	return FHyperAIStudioPhysicsContracts::InspectResultTypeId;
}

FString FHyperAIStudioPhysicsInspectResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPhysicsContracts::InspectResultSchemaFingerprint();
}

int32 FHyperAIStudioPhysicsInspectResultPayload::GetBoundedByteSize() const
{
	int64 Bytes = HyperAIStudio::Physics::Private::EstimatedBaseReportBytes;
	for (const FHyperAIPhysicsBodyRecord& Record : Report.Bodies)
	{
		Bytes = HyperAIStudio::Physics::Private::SaturatingAdd(Bytes,
			HyperAIStudio::Physics::Private::EstimateBodyRecordBytes(Record));
	}
	for (const FHyperAIPhysicsConstraintRecord& Record : Report.Constraints)
	{
		Bytes = HyperAIStudio::Physics::Private::SaturatingAdd(Bytes,
			HyperAIStudio::Physics::Private::EstimateConstraintRecordBytes(Record));
	}
	for (const FHyperAIPhysicsCollisionPairRecord& Record : Report.DisabledCollisionPairs)
	{
		Bytes = HyperAIStudio::Physics::Private::SaturatingAdd(Bytes,
			HyperAIStudio::Physics::Private::EstimatePairRecordBytes(Record));
	}
	return static_cast<int32>(FMath::Min<int64>(Bytes, MAX_int32));
}

FString FHyperAIStudioPhysicsValidateResultPayload::GetTypeId() const
{
	return FHyperAIStudioPhysicsContracts::ValidateResultTypeId;
}

FString FHyperAIStudioPhysicsValidateResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioPhysicsContracts::ValidateResultSchemaFingerprint();
}

int32 FHyperAIStudioPhysicsValidateResultPayload::GetBoundedByteSize() const
{
	int64 Bytes = 4096;
	for (const FHyperAIPhysicsIssue& Issue : Report.Issues)
	{
		Bytes = HyperAIStudio::Physics::Private::SaturatingAdd(Bytes,
			HyperAIStudio::Physics::Private::EstimateIssueBytes(Issue));
	}
	return static_cast<int32>(FMath::Min<int64>(Bytes, MAX_int32));
}

bool FHyperAIStudioPhysicsContracts::CaptureExact(const FString& TargetPath,
	const int32 MaxWorkMs, FHyperAIStudioPhysicsValueSnapshot& OutSnapshot,
	FString& OutStatus, FString& OutDiagnostic)
{
	using namespace HyperAIStudio::Physics::Private;
	OutSnapshot = {};
	OutStatus.Reset();
	OutDiagnostic.Reset();
	auto Fail = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		OutStatus = Status;
		OutDiagnostic = Diagnostic;
		return false;
	};
	if (!IsInGameThread())
	{
		return Fail(TEXT("game_thread_required"),
			TEXT("Exact loaded physics capture is game-thread only."));
	}
	if (!IsCanonicalProjectObjectPath(TargetPath) || MaxWorkMs < 1
		|| MaxWorkMs > MaxReadGameThreadMs)
	{
		return Fail(TEXT("invalid_capture_bounds"),
			TEXT("Capture requires one canonical top-level /Game object path and a closed work budget."));
	}
	const double DeadlineSeconds = FPlatformTime::Seconds() + MaxWorkMs / 1000.0;
	UObject* Resolved = FSoftObjectPath(TargetPath).ResolveObject();
	if (!Resolved)
	{
		return Fail(TEXT("target_not_loaded"),
			TEXT("The exact PhysicsAsset is not already loaded; HyperAI never loads, opens, searches, or scans for it."));
	}
	UPhysicsAsset* Asset = Cast<UPhysicsAsset>(Resolved);
	if (!Asset || Asset->GetClass() != UPhysicsAsset::StaticClass()
		|| Asset->GetPathName() != TargetPath)
	{
		return Fail(TEXT("target_class_mismatch"),
			TEXT("The exact loaded top-level object is not a UPhysicsAsset."));
	}
	UPackage* Package = Asset->GetOutermost();
	if (!Package || Package == GetTransientPackage()
		|| Package->HasAnyPackageFlags(PKG_InMemoryOnly | PKG_PlayInEditor))
	{
		return Fail(TEXT("unsupported_target_package"),
			TEXT("Transient and PIE packages are outside the persisted physics CAS contract."));
	}

	const int32 BodyCount = Asset->SkeletalBodySetups.Num();
	const int32 ConstraintCount = Asset->ConstraintSetup.Num();
	const int32 PairCount = Asset->CollisionDisableTable.Num();
	if (BodyCount < 0 || BodyCount > MaxBodies || ConstraintCount < 0
		|| ConstraintCount > MaxConstraints || PairCount < 0
		|| PairCount > MaxDisabledCollisionPairs)
	{
		return Fail(TEXT("asset_container_bound_exceeded"),
			TEXT("Body, constraint, or disabled-pair count exceeded its hard bound before materialization."));
	}

	FHyperAIPhysicsAssetRecord& Record = OutSnapshot.Asset;
	Record.TargetPath = TargetPath;
	Record.ClassPath = Asset->GetClass()->GetPathName();
	Record.PackageName = Package->GetName();
	Record.bLoaded = true;
	Record.bWasLoadedFromDisk = Asset->HasAnyFlags(RF_WasLoaded);
	Record.bPackageDirty = Package->IsDirty();
	Record.BodyCount = BodyCount;
	Record.ConstraintCount = ConstraintCount;
	Record.DisabledCollisionPairCount = PairCount;

	bool bComplete = true;
	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	FAssetPackageData PackageData;
	UE::AssetRegistry::EExists PackageState = UE::AssetRegistry::EExists::Unknown;
	if (AssetRegistry)
	{
		PackageState = AssetRegistry->TryGetAssetPackageData(
			Package->GetFName(), PackageData, /*bFailIfLockHeld=*/true);
	}
	Record.DiskExistence = ClassifyAssetRegistryExistence(PackageState);
	if (!AssetRegistry || PackageState != UE::AssetRegistry::EExists::Exists
		|| !Record.bWasLoadedFromDisk)
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("asset_registry_presence_unproven"),
			TEXT("error"), TargetPath,
			TEXT("TryGetAssetPackageData(..., true) must return Exists and the exact object must carry RF_WasLoaded; Unknown is never absence."));
	}
	else
	{
		Record.DiskSize = PackageData.DiskSize;
		Record.PackageSavedHash = LexToString(PackageData.GetPackageSavedHash());
		if (Record.DiskSize <= 0 || PackageData.GetPackageSavedHash().IsZero()
			|| Record.PackageSavedHash.IsEmpty())
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("saved_package_identity_unavailable"),
				TEXT("error"), TargetPath,
				TEXT("Asset Registry presence lacks a nonzero package saved hash or disk size."));
		}
	}
	if (Record.bPackageDirty)
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("dirty_loaded_state_not_cas_complete"),
			TEXT("error"), TargetPath,
			TEXT("Dirty loaded physics values cannot claim a persisted CAS revision."));
	}

	const FPhysicsAssetSolverSettings& SolverSettings = Asset->SolverSettings;
	Record.Solver.PositionIterations = SolverSettings.PositionIterations;
	Record.Solver.VelocityIterations = SolverSettings.VelocityIterations;
	Record.Solver.ProjectionIterations = SolverSettings.ProjectionIterations;
	Record.Solver.CullDistance = SolverSettings.CullDistance;
	Record.Solver.MaxDepenetrationVelocity = SolverSettings.MaxDepenetrationVelocity;
	Record.Solver.FixedTimeStep = SolverSettings.FixedTimeStep;
	Record.Solver.bUseLinearJointSolver = SolverSettings.bUseLinearJointSolver;
	Record.Solver.bUseManifolds = SolverSettings.bUseManifolds;
	Record.Solver.Fingerprint = BuildSolverFingerprint(Record.Solver);
	if (!IsCanonicalSha256(Record.Solver.Fingerprint)
		|| !IsFinite(Record.Solver.CullDistance)
		|| !IsFinite(Record.Solver.MaxDepenetrationVelocity)
		|| !IsFinite(Record.Solver.FixedTimeStep))
	{
		bComplete = false;
		AddIssue(OutSnapshot.CaptureIssues, TEXT("solver_projection_incomplete"),
			TEXT("error"), TargetPath,
			TEXT("Solver fields could not be projected into one complete finite value record."));
	}

	OutSnapshot.Bodies.Reserve(BodyCount);
	TSet<FString> BodyNames;
	for (int32 Index = 0; Index < BodyCount; ++Index)
	{
		if (DeadlineExceeded(DeadlineSeconds))
		{
			return Fail(TEXT("capture_deadline_exceeded"),
				TEXT("Physics capture exceeded its closed game-thread deadline."));
		}
		USkeletalBodySetup* Setup = Asset->SkeletalBodySetups[Index].Get();
		if (!Setup)
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("null_body_setup"), TEXT("error"),
				FString::FromInt(Index), TEXT("The bounded PhysicsAsset array contains a null body setup."));
			continue;
		}
		if (Setup->GetClass() != USkeletalBodySetup::StaticClass())
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("derived_body_setup_unsupported"),
				TEXT("error"), Setup->GetClass()->GetPathName(),
				TEXT("The v1 public projection admits exact USkeletalBodySetup instances only."));
			continue;
		}
		const FString BoneName = Setup->BoneName.ToString();
		const FString SetupPath = PhysicsObjectPath(Setup);
		const FString PhysicalMaterialPath = PhysicsObjectPath(Setup->PhysMaterial.Get());
		if (!IsSafeName(BoneName) || SetupPath.IsEmpty()
			|| SetupPath.Len() > MaxPathCharacters || BodyNames.Contains(BoneName))
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("invalid_or_duplicate_body_identity"),
				TEXT("error"), BoneName,
				TEXT("Each body requires one unique bounded canonical bone identity and loaded object path."));
			continue;
		}
		BodyNames.Add(BoneName);

		const FKAggregateGeom& Geometry = Setup->AggGeom;
		const int64 TotalShapes = static_cast<int64>(Geometry.SphereElems.Num())
			+ Geometry.BoxElems.Num() + Geometry.SphylElems.Num()
			+ Geometry.ConvexElems.Num() + Geometry.TaperedCapsuleElems.Num()
			+ Geometry.LevelSetElems.Num() + Geometry.SkinnedLevelSetElems.Num()
			+ Geometry.MLLevelSetElems.Num() + Geometry.SkinnedTriangleMeshElems.Num();
		if (TotalShapes < 0 || TotalShapes > MaxShapesPerBody)
		{
			return Fail(TEXT("shape_container_bound_exceeded"),
				TEXT("A body shape count exceeded its hard bound before any geometry materialization."));
		}

		FProjectionContext Context;
		if (PhysicalMaterialPath.Len() > MaxPathCharacters)
		{
			Context.MarkIncomplete(TEXT("physical_material_path_too_long"));
		}
		FString ShapesFingerprint;
		int32 InvalidShapeCount = 0;
		int32 UnsupportedShapeCount = 0;
		BuildShapesFingerprint(Geometry, Context, ShapesFingerprint,
			InvalidShapeCount, UnsupportedShapeCount);
		const FString DefaultInstanceFingerprint =
			BuildDefaultInstanceFingerprint(Setup->DefaultInstance, Context);

		FString BodyCanonical;
		Context.Append(BodyCanonical, TEXT("hyperai.physics.body.v1"),
			TEXT("body_projection_exceeded"));
		Context.Append(BodyCanonical, BoneName, TEXT("body_name_projection_exceeded"));
		Context.Append(BodyCanonical, SetupPath, TEXT("body_path_projection_exceeded"));
		Context.Append(BodyCanonical,
			Setup->BodySetupGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
			TEXT("body_guid_projection_exceeded"));
		Context.Append(BodyCanonical, PhysicalMaterialPath,
			TEXT("physical_material_projection_exceeded"));
		Context.Append(BodyCanonical, FString::FromInt(static_cast<int32>(Setup->PhysicsType)),
			TEXT("physics_type_projection_exceeded"));
		Context.Append(BodyCanonical,
			FString::FromInt(static_cast<int32>(Setup->GetCollisionTraceFlag())),
			TEXT("collision_trace_projection_exceeded"));
		Context.Append(BodyCanonical,
			FString::FromInt(static_cast<int32>(Setup->CollisionReponse)),
			TEXT("collision_response_projection_exceeded"));
		Context.Append(BodyCanonical, Setup->bConsiderForBounds ? TEXT("bounds") : TEXT("no_bounds"),
			TEXT("body_bounds_projection_exceeded"));
		Context.Append(BodyCanonical, DefaultInstanceFingerprint,
			TEXT("default_instance_projection_exceeded"));
		Context.Append(BodyCanonical, ShapesFingerprint, TEXT("shape_projection_exceeded"));

		FHyperAIPhysicsBodyRecord Body;
		Body.BoneName = BoneName;
		Body.BodyObjectPath = SetupPath;
		Body.StableId = HashCanonical(
			FString(TEXT("hyperai.physics.body-id.v1|")) + BoneName);
		Body.BodyFingerprint = HashCanonical(BodyCanonical);
		Body.DefaultInstanceFingerprint = DefaultInstanceFingerprint;
		Body.BodySetupGuid = Setup->BodySetupGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
		Body.PhysicalMaterialPath = PhysicalMaterialPath.Left(MaxPathCharacters);
		Body.CollisionProfileName = Setup->DefaultInstance.GetCollisionProfileName().ToString()
			.Left(MaxNameCharacters);
		Body.PhysicsType = static_cast<int32>(Setup->PhysicsType);
		Body.CollisionTraceFlag = static_cast<int32>(Setup->GetCollisionTraceFlag());
		Body.BodyCollisionResponse = static_cast<int32>(Setup->CollisionReponse);
		Body.CollisionEnabled = static_cast<int32>(Setup->DefaultInstance.GetCollisionEnabled(false));
		Body.MassScale = Setup->DefaultInstance.MassScale;
		Body.bSimulatePhysics = Setup->DefaultInstance.bSimulatePhysics;
		Body.bConsiderForBounds = Setup->bConsiderForBounds;
		Body.bShapeProjectionComplete = Context.bComplete && UnsupportedShapeCount == 0;
		Body.bShapeGeometryValid = InvalidShapeCount == 0 && UnsupportedShapeCount == 0;
		Body.SphereCount = Geometry.SphereElems.Num();
		Body.BoxCount = Geometry.BoxElems.Num();
		Body.CapsuleCount = Geometry.SphylElems.Num();
		Body.ConvexCount = Geometry.ConvexElems.Num();
		Body.TaperedCapsuleCount = Geometry.TaperedCapsuleElems.Num();
		Body.UnsupportedShapeCount = UnsupportedShapeCount;
		Body.TotalShapeCount = static_cast<int32>(TotalShapes);
		Body.InvalidShapeCount = InvalidShapeCount;
		if (!Context.bComplete || !IsCanonicalSha256(Body.StableId)
			|| !IsCanonicalSha256(Body.BodyFingerprint)
			|| !IsCanonicalSha256(DefaultInstanceFingerprint))
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("body_projection_incomplete"),
				TEXT("error"), BoneName,
				Context.Reasons.IsEmpty() ? TEXT("Body semantic projection is incomplete.")
					: FString::Join(Context.Reasons, TEXT(",")));
		}
		OutSnapshot.Bodies.Add(MoveTemp(Body));
	}

	OutSnapshot.Constraints.Reserve(ConstraintCount);
	TSet<FString> ConstraintIds;
	for (int32 Index = 0; Index < ConstraintCount; ++Index)
	{
		if (DeadlineExceeded(DeadlineSeconds))
		{
			return Fail(TEXT("capture_deadline_exceeded"),
				TEXT("Physics constraint capture exceeded its closed game-thread deadline."));
		}
		UPhysicsConstraintTemplate* Template = Asset->ConstraintSetup[Index].Get();
		if (!Template)
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("null_constraint_template"), TEXT("error"),
				FString::FromInt(Index), TEXT("The bounded constraint array contains a null template."));
			continue;
		}
		if (Template->GetClass() != UPhysicsConstraintTemplate::StaticClass())
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("derived_constraint_template_unsupported"),
				TEXT("error"), Template->GetClass()->GetPathName(),
				TEXT("The v1 public projection admits exact UPhysicsConstraintTemplate instances only."));
			continue;
		}
		const FConstraintInstance& Instance = Template->DefaultInstance;
		const FString Child = Instance.GetChildBoneName().ToString();
		const FString Parent = Instance.GetParentBoneName().ToString();
		const FString JointName = Instance.JointName.ToString();
		const FString TemplatePath = PhysicsObjectPath(Template);
		FString Identity;
		AppendTokenUnchecked(Identity, TEXT("hyperai.physics.constraint-id.v1"));
		AppendTokenUnchecked(Identity, Child);
		AppendTokenUnchecked(Identity, Parent);
		AppendTokenUnchecked(Identity, JointName);
		AppendTokenUnchecked(Identity, TemplatePath);
		const FString StableId = HashCanonical(Identity);
		FProjectionContext Context;
		FHyperAIPhysicsConstraintRecord Constraint;
		Constraint.StableId = StableId;
		Constraint.TemplateObjectPath = TemplatePath;
		Constraint.ConstraintFingerprint = BuildConstraintFingerprint(Instance, Context);
		Constraint.ChildBoneName = Child;
		Constraint.ParentBoneName = Parent;
		Constraint.LinearXMotion = static_cast<int32>(Instance.GetLinearXMotion());
		Constraint.LinearYMotion = static_cast<int32>(Instance.GetLinearYMotion());
		Constraint.LinearZMotion = static_cast<int32>(Instance.GetLinearZMotion());
		Constraint.Swing1Motion = static_cast<int32>(Instance.GetAngularSwing1Motion());
		Constraint.Swing2Motion = static_cast<int32>(Instance.GetAngularSwing2Motion());
		Constraint.TwistMotion = static_cast<int32>(Instance.GetAngularTwistMotion());
		Constraint.LinearLimit = Instance.GetLinearLimit();
		Constraint.Swing1LimitDegrees = Instance.GetAngularSwing1Limit();
		Constraint.Swing2LimitDegrees = Instance.GetAngularSwing2Limit();
		Constraint.TwistLimitDegrees = Instance.GetAngularTwistLimit();
		Constraint.bDisableCollision = Instance.ProfileInstance.bDisableCollision;
		if (!IsSafeName(Child) || !IsSafeName(Parent)
			|| (!JointName.IsEmpty() && !IsSafeName(JointName, true)) || TemplatePath.IsEmpty()
			|| TemplatePath.Len() > MaxPathCharacters || ConstraintIds.Contains(StableId)
			|| !Context.bComplete || !IsCanonicalSha256(StableId)
			|| !IsCanonicalSha256(Constraint.ConstraintFingerprint))
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("constraint_projection_incomplete"),
				TEXT("error"), StableId,
				TEXT("Constraint identity or public value projection is incomplete or duplicated."));
		}
		ConstraintIds.Add(StableId);
		OutSnapshot.Constraints.Add(MoveTemp(Constraint));
	}

	OutSnapshot.DisabledCollisionPairs.Reserve(PairCount);
	for (const TPair<FRigidBodyIndexPair, bool>& Entry : Asset->CollisionDisableTable)
	{
		if (DeadlineExceeded(DeadlineSeconds))
		{
			return Fail(TEXT("capture_deadline_exceeded"),
				TEXT("Physics collision-pair capture exceeded its closed game-thread deadline."));
		}
		const int32 A = Entry.Key.Indices[0];
		const int32 B = Entry.Key.Indices[1];
		if (!Asset->SkeletalBodySetups.IsValidIndex(A)
			|| !Asset->SkeletalBodySetups.IsValidIndex(B)
			|| !Asset->SkeletalBodySetups[A] || !Asset->SkeletalBodySetups[B])
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("collision_pair_index_invalid"),
				TEXT("error"), FString::Printf(TEXT("%d:%d"), A, B),
				TEXT("A disabled-collision pair does not close over the bounded body array."));
			continue;
		}
		FHyperAIPhysicsCollisionPairRecord Pair;
		Pair.BodyIndexA = A;
		Pair.BodyIndexB = B;
		Pair.BodyNameA = Asset->SkeletalBodySetups[A]->BoneName.ToString();
		Pair.BodyNameB = Asset->SkeletalBodySetups[B]->BoneName.ToString();
		Pair.bDisabled = Entry.Value;
		if (!IsSafeName(Pair.BodyNameA) || !IsSafeName(Pair.BodyNameB))
		{
			bComplete = false;
			AddIssue(OutSnapshot.CaptureIssues, TEXT("collision_pair_name_invalid"),
				TEXT("error"), FString::Printf(TEXT("%d:%d"), A, B),
				TEXT("A disabled collision pair references an invalid bounded body name."));
			Pair.BodyNameA = Pair.BodyNameA.Left(MaxNameCharacters);
			Pair.BodyNameB = Pair.BodyNameB.Left(MaxNameCharacters);
		}
		FString Canonical;
		AppendTokenUnchecked(Canonical, TEXT("hyperai.physics.collision-pair.v1"));
		AppendTokenUnchecked(Canonical, Pair.BodyNameA);
		AppendTokenUnchecked(Canonical, Pair.BodyNameB);
		AppendTokenUnchecked(Canonical, Pair.bDisabled ? TEXT("disabled") : TEXT("enabled"));
		Pair.StableId = HashCanonical(Canonical);
		OutSnapshot.DisabledCollisionPairs.Add(MoveTemp(Pair));
	}

	OutSnapshot.Bodies.Sort([](const FHyperAIPhysicsBodyRecord& A,
		const FHyperAIPhysicsBodyRecord& B) { return A.StableId < B.StableId; });
	OutSnapshot.Constraints.Sort([](const FHyperAIPhysicsConstraintRecord& A,
		const FHyperAIPhysicsConstraintRecord& B) { return A.StableId < B.StableId; });
	OutSnapshot.DisabledCollisionPairs.Sort([](const FHyperAIPhysicsCollisionPairRecord& A,
		const FHyperAIPhysicsCollisionPairRecord& B) { return A.StableId < B.StableId; });

	FString PersistedCanonical;
	AppendTokenUnchecked(PersistedCanonical, TEXT("hyperai.physics.persisted.v1"));
	AppendTokenUnchecked(PersistedCanonical, Record.TargetPath);
	AppendTokenUnchecked(PersistedCanonical, Record.ClassPath);
	AppendTokenUnchecked(PersistedCanonical, Record.PackageName);
	AppendTokenUnchecked(PersistedCanonical, Record.PackageSavedHash);
	AppendTokenUnchecked(PersistedCanonical, FString::Printf(TEXT("%lld"), Record.DiskSize));
	AppendTokenUnchecked(PersistedCanonical, Record.Solver.Fingerprint);
	AppendTokenUnchecked(PersistedCanonical, FString::FromInt(OutSnapshot.Bodies.Num()));
	for (const FHyperAIPhysicsBodyRecord& Body : OutSnapshot.Bodies)
	{
		AppendTokenUnchecked(PersistedCanonical, Body.StableId);
		AppendTokenUnchecked(PersistedCanonical, Body.BodyFingerprint);
	}
	AppendTokenUnchecked(PersistedCanonical, FString::FromInt(OutSnapshot.Constraints.Num()));
	for (const FHyperAIPhysicsConstraintRecord& Constraint : OutSnapshot.Constraints)
	{
		AppendTokenUnchecked(PersistedCanonical, Constraint.StableId);
		AppendTokenUnchecked(PersistedCanonical, Constraint.ConstraintFingerprint);
	}
	AppendTokenUnchecked(PersistedCanonical,
		FString::FromInt(OutSnapshot.DisabledCollisionPairs.Num()));
	for (const FHyperAIPhysicsCollisionPairRecord& Pair : OutSnapshot.DisabledCollisionPairs)
	{
		AppendTokenUnchecked(PersistedCanonical, Pair.StableId);
	}
	const FString ProjectedValueFingerprint = HashCanonical(PersistedCanonical);
	if (bComplete && IsCanonicalSha256(ProjectedValueFingerprint))
	{
		Record.PersistedRevision = ProjectedValueFingerprint;
		Record.bRevisionComplete = true;
	}
	else
	{
		bComplete = false;
	}

	FString VolatileCanonical;
	AppendTokenUnchecked(VolatileCanonical, TEXT("hyperai.physics.volatile-observation.v1"));
	// The volatile seal retains the bounded current value projection even when
	// dirty/unknown/unsupported evidence prevents promotion to persisted CAS.
	AppendTokenUnchecked(VolatileCanonical, ProjectedValueFingerprint);
	AppendTokenUnchecked(VolatileCanonical, Record.bLoaded ? TEXT("loaded") : TEXT("unloaded"));
	AppendTokenUnchecked(VolatileCanonical,
		Record.bWasLoadedFromDisk ? TEXT("was_loaded") : TEXT("not_disk_loaded"));
	AppendTokenUnchecked(VolatileCanonical, Record.bPackageDirty ? TEXT("dirty") : TEXT("clean"));
	AppendTokenUnchecked(VolatileCanonical, Record.DiskExistence);
	Record.VolatileObservationFingerprint = HashCanonical(VolatileCanonical);
	OutSnapshot.bComplete = bComplete && Record.bRevisionComplete;
	if (!OutSnapshot.bComplete)
	{
		Record.bRevisionComplete = false;
		Record.PersistedRevision.Reset();
	}
	OutStatus = OutSnapshot.bComplete ? TEXT("exact_loaded_physics_snapshot")
		: TEXT("snapshot_incomplete");
	OutDiagnostic = OutSnapshot.bComplete
		? TEXT("Captured one bounded, clean, already-loaded UPhysicsAsset with non-blocking Asset Registry CAS evidence; no object was loaded or opened.")
		: TEXT("Loaded public values were inspected, but dirty, unknown, unsupported, invalid, or incomplete evidence prevents persisted revision admission.");
	return true;
}

bool FHyperAIStudioPhysicsContracts::ValidateDetached(
	const FHyperAIStudioPhysicsValueSnapshot& Snapshot, const FString& Policy,
	const int32 MaxIssueCount, const int32 OutputByteLimit,
	FHyperAIPhysicsValidateReport& OutReport)
{
	using namespace HyperAIStudio::Physics::Private;
	OutReport = {};
	OutReport.Policy = Policy.Left(32);
	OutReport.PersistedRevision = Snapshot.Asset.PersistedRevision;
	OutReport.Capabilities = GetCapabilityMatrix();
	if ((Policy != TEXT("structural") && Policy != TEXT("simulation_ready"))
		|| MaxIssueCount < 1 || MaxIssueCount > MaxIssues
		|| OutputByteLimit < MinOutputBytes || OutputByteLimit > MaxOutputBytes)
	{
		OutReport.Status = TEXT("invalid_validation_bounds_or_policy");
		OutReport.Diagnostic = TEXT("Detached validation accepts only structural/simulation_ready and closed issue/output bounds.");
		return false;
	}

	int64 EstimatedBytes = 8192;
	bool bBudgetComplete = true;
	auto Emit = [&](const FString& Code, const FString& Severity,
		const FString& Subject, const FString& Detail)
	{
		FHyperAIPhysicsIssue Issue;
		TArray<FHyperAIPhysicsIssue> One;
		AddIssue(One, Code, Severity, Subject, Detail);
		if (One.IsEmpty())
		{
			bBudgetComplete = false;
			return false;
		}
		Issue = MoveTemp(One[0]);
		const int64 IssueBytes = EstimateIssueBytes(Issue);
		if (OutReport.Issues.Num() >= MaxIssueCount
			|| EstimatedBytes > OutputByteLimit - IssueBytes)
		{
			bBudgetComplete = false;
			OutReport.bTruncated = true;
			return false;
		}
		EstimatedBytes += IssueBytes;
		if (Severity == TEXT("error"))
		{
			++OutReport.ErrorCount;
		}
		else if (Severity == TEXT("warning"))
		{
			++OutReport.WarningCount;
		}
		OutReport.Issues.Add(MoveTemp(Issue));
		return true;
	};

	if (!Snapshot.bComplete || !Snapshot.Asset.bRevisionComplete
		|| !IsCanonicalSha256(Snapshot.Asset.PersistedRevision))
	{
		Emit(TEXT("persisted_revision_incomplete"), TEXT("error"),
			Snapshot.Asset.TargetPath,
			TEXT("Independent validation cannot admit a value snapshot without complete clean persisted CAS evidence."));
	}
	for (const FHyperAIPhysicsIssue& CaptureIssue : Snapshot.CaptureIssues)
	{
		if (!Emit(CaptureIssue.Code, CaptureIssue.Severity,
			CaptureIssue.Subject, CaptureIssue.Detail))
		{
			break;
		}
	}
	if (Snapshot.Asset.BodyCount != Snapshot.Bodies.Num()
		|| Snapshot.Asset.ConstraintCount != Snapshot.Constraints.Num()
		|| Snapshot.Asset.DisabledCollisionPairCount != Snapshot.DisabledCollisionPairs.Num())
	{
		Emit(TEXT("record_count_mismatch"), TEXT("error"), Snapshot.Asset.TargetPath,
			TEXT("Detached record arrays do not exactly match the captured top-level counts."));
	}
	if (Snapshot.Bodies.IsEmpty())
	{
		Emit(TEXT("physics_asset_has_no_bodies"), TEXT("error"), Snapshot.Asset.TargetPath,
			TEXT("A structurally useful PhysicsAsset requires at least one bounded body."));
	}

	TSet<FString> BodyNames;
	TSet<FString> BodyIds;
	for (const FHyperAIPhysicsBodyRecord& Body : Snapshot.Bodies)
	{
		FGuid BodyGuid;
		const bool bBodyGuidValid = FGuid::ParseExact(Body.BodySetupGuid,
			EGuidFormats::DigitsWithHyphensLower, BodyGuid) && BodyGuid.IsValid()
			&& BodyGuid.ToString(EGuidFormats::DigitsWithHyphensLower) == Body.BodySetupGuid;
		if (!IsSafeName(Body.BoneName) || BodyNames.Contains(Body.BoneName)
			|| !IsCanonicalSha256(Body.StableId) || BodyIds.Contains(Body.StableId)
			|| !IsCanonicalSha256(Body.BodyFingerprint)
			|| !IsCanonicalSha256(Body.DefaultInstanceFingerprint) || !bBodyGuidValid
			|| Body.PhysicsType < 0 || Body.PhysicsType > 2
			|| Body.CollisionTraceFlag < 0 || Body.CollisionTraceFlag >= CTF_MAX
			|| Body.BodyCollisionResponse < 0 || Body.BodyCollisionResponse > 1
			|| Body.CollisionEnabled < 0 || Body.CollisionEnabled > 5)
		{
			if (!Emit(TEXT("body_identity_invalid"), TEXT("error"), Body.BoneName,
				TEXT("Body names, stable ids, and value fingerprints must be unique and canonical.")))
			{
				break;
			}
		}
		BodyNames.Add(Body.BoneName);
		BodyIds.Add(Body.StableId);
		if (!Body.bShapeProjectionComplete || Body.UnsupportedShapeCount != 0)
		{
			if (!Emit(TEXT("body_shape_projection_incomplete"), TEXT("error"), Body.BoneName,
				TEXT("Level-set/skinned geometry or another unprojectable shape prevents complete value validation.")))
			{
				break;
			}
		}
		const int64 ProjectedShapeCount = static_cast<int64>(Body.SphereCount)
			+ Body.BoxCount + Body.CapsuleCount + Body.ConvexCount
			+ Body.TaperedCapsuleCount + Body.UnsupportedShapeCount;
		if (!Body.bShapeGeometryValid || Body.InvalidShapeCount != 0
			|| Body.TotalShapeCount <= 0 || Body.TotalShapeCount > MaxShapesPerBody
			|| Body.SphereCount < 0 || Body.BoxCount < 0 || Body.CapsuleCount < 0
			|| Body.ConvexCount < 0 || Body.TaperedCapsuleCount < 0
			|| Body.UnsupportedShapeCount < 0 || ProjectedShapeCount != Body.TotalShapeCount)
		{
			if (!Emit(TEXT("body_shape_geometry_invalid"), TEXT("error"), Body.BoneName,
				TEXT("Each body requires at least one bounded, finite, positive-size collision shape.")))
			{
				break;
			}
		}
		if (!IsFinite(Body.MassScale) || Body.MassScale <= 0.0)
		{
			if (!Emit(TEXT("body_mass_scale_invalid"), TEXT("error"), Body.BoneName,
				TEXT("The detached mass scale must be finite and greater than zero.")))
			{
				break;
			}
		}
	}

	const FHyperAIPhysicsSolverRecord& Solver = Snapshot.Asset.Solver;
	if (!IsCanonicalSha256(Solver.Fingerprint)
		|| Solver.PositionIterations < 1 || Solver.PositionIterations > 50
		|| Solver.VelocityIterations < 0 || Solver.VelocityIterations > 50
		|| Solver.ProjectionIterations < 0 || Solver.ProjectionIterations > 50
		|| !IsFinite(Solver.CullDistance) || Solver.CullDistance < 0.0
		|| !IsFinite(Solver.MaxDepenetrationVelocity)
		|| Solver.MaxDepenetrationVelocity < 0.0
		|| !IsFinite(Solver.FixedTimeStep) || Solver.FixedTimeStep < 0.0
		|| Solver.FixedTimeStep > 1.0)
	{
		Emit(TEXT("solver_settings_invalid"), TEXT("error"), Snapshot.Asset.TargetPath,
			TEXT("Solver iterations and distances must be finite and remain within the closed UE 5.8 preflight envelope."));
	}

	TSet<FString> ConstraintIds;
	for (const FHyperAIPhysicsConstraintRecord& Constraint : Snapshot.Constraints)
	{
		if (!IsCanonicalSha256(Constraint.StableId)
			|| !IsCanonicalSha256(Constraint.ConstraintFingerprint)
			|| ConstraintIds.Contains(Constraint.StableId)
			|| !BodyNames.Contains(Constraint.ChildBoneName)
			|| !BodyNames.Contains(Constraint.ParentBoneName)
			|| Constraint.ChildBoneName == Constraint.ParentBoneName
			|| Constraint.LinearXMotion < 0 || Constraint.LinearXMotion > 2
			|| Constraint.LinearYMotion < 0 || Constraint.LinearYMotion > 2
			|| Constraint.LinearZMotion < 0 || Constraint.LinearZMotion > 2
			|| Constraint.Swing1Motion < 0 || Constraint.Swing1Motion > 2
			|| Constraint.Swing2Motion < 0 || Constraint.Swing2Motion > 2
			|| Constraint.TwistMotion < 0 || Constraint.TwistMotion > 2
			|| !IsFinite(Constraint.LinearLimit) || Constraint.LinearLimit < 0.0
			|| !IsFinite(Constraint.Swing1LimitDegrees)
			|| Constraint.Swing1LimitDegrees < 0.0
			|| !IsFinite(Constraint.Swing2LimitDegrees)
			|| Constraint.Swing2LimitDegrees < 0.0
			|| !IsFinite(Constraint.TwistLimitDegrees)
			|| Constraint.TwistLimitDegrees < 0.0)
		{
			if (!Emit(TEXT("constraint_closure_invalid"), TEXT("error"),
				Constraint.StableId,
				TEXT("Each unique constraint must close over two distinct body names and finite public limit values.")))
			{
				break;
			}
		}
		ConstraintIds.Add(Constraint.StableId);
	}

	TSet<FString> PairIds;
	for (const FHyperAIPhysicsCollisionPairRecord& Pair : Snapshot.DisabledCollisionPairs)
	{
		if (!IsCanonicalSha256(Pair.StableId) || PairIds.Contains(Pair.StableId)
			|| !BodyNames.Contains(Pair.BodyNameA) || !BodyNames.Contains(Pair.BodyNameB)
			|| Pair.BodyNameA == Pair.BodyNameB || !Pair.bDisabled)
		{
			if (!Emit(TEXT("disabled_collision_pair_invalid"), TEXT("error"), Pair.StableId,
				TEXT("Each projected disabled pair must uniquely close over two distinct captured bodies.")))
			{
				break;
			}
		}
		PairIds.Add(Pair.StableId);
	}
	if (Policy == TEXT("simulation_ready"))
	{
		Emit(TEXT("preview_simulation_evidence_required"), TEXT("error"),
			Snapshot.Asset.TargetPath,
			TEXT("Simulation-ready validity requires bounded compile/simulation backend evidence; this detached validator never simulates."));
	}

	FString Canonical;
	AppendTokenUnchecked(Canonical, TEXT("hyperai.physics.detached-validator.v1"));
	AppendTokenUnchecked(Canonical, Policy);
	AppendTokenUnchecked(Canonical, Snapshot.Asset.PersistedRevision);
	AppendTokenUnchecked(Canonical, FString::FromInt(OutReport.ErrorCount));
	AppendTokenUnchecked(Canonical, FString::FromInt(OutReport.WarningCount));
	AppendTokenUnchecked(Canonical, bBudgetComplete ? TEXT("complete") : TEXT("truncated"));
	for (const FHyperAIPhysicsIssue& Issue : OutReport.Issues)
	{
		AppendTokenUnchecked(Canonical, Issue.StableId);
	}
	OutReport.ValidatorFingerprint = HashCanonical(Canonical);
	OutReport.bComplete = Snapshot.bComplete && bBudgetComplete
		&& Policy == TEXT("structural") && IsCanonicalSha256(OutReport.ValidatorFingerprint);
	OutReport.bValid = OutReport.bComplete && OutReport.ErrorCount == 0;
	OutReport.bOk = true;
	OutReport.Status = OutReport.bValid ? TEXT("valid")
		: (OutReport.bComplete ? TEXT("invalid") : TEXT("validation_incomplete"));
	OutReport.Diagnostic = OutReport.bValid
		? TEXT("Detached bounded public-value validation passed without object access or simulation.")
		: TEXT("Detached validation found structural errors or lacks complete persisted/simulation/budget evidence.");
	return true;
}

FHyperAIPhysicsInspectReport FHyperAIStudioPhysicsContracts::Inspect(
	const FHyperAIPhysicsInspectRequest& Request)
{
	using namespace HyperAIStudio::Physics::Private;
	FHyperAIPhysicsInspectReport Report;
	Report.Capabilities = GetCapabilityMatrix();
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.bOk = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		return Reject(TEXT("source_candidate_dev_mode_required"),
			TEXT("The exact Physics source cohort is SourceCandidate-only and remains unavailable outside the generated development gate."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath)
		|| Request.PageSize < 1 || Request.PageSize > MaxPageSize
		|| Request.Cursor.Len() > MaxCursorCharacters
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > MaxReadGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_inspect_bounds"),
			TEXT("Inspect requires canonical exact identity and closed page/cursor/work/output bounds."));
	}
	FHyperAIStudioPhysicsValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.TargetPath, Request.MaxGameThreadMs, Snapshot,
		CaptureStatus, CaptureDiagnostic))
	{
		return Reject(*CaptureStatus, *CaptureDiagnostic);
	}
	Report.Asset = Snapshot.Asset;
	Report.bFreshCapture = true;
	Report.Issues = Snapshot.CaptureIssues;
	Report.bCursorEligible = Snapshot.bComplete && Snapshot.Asset.bRevisionComplete;
	if (!Request.Cursor.IsEmpty() && !Report.bCursorEligible)
	{
		return Reject(TEXT("cursor_ineligible"),
			TEXT("Paging cursors are unavailable when the persisted physics revision is incomplete."));
	}
	int32 Offset = 0;
	if (Report.bCursorEligible && !ParseCursor(Request.Cursor, Request.TargetPath,
		Snapshot.Asset.PersistedRevision, Request.PageSize, Offset))
	{
		return Reject(TEXT("invalid_or_stale_cursor"),
			TEXT("The inspect cursor is malformed or sealed to different identity, revision, or bounds."));
	}
	const int32 Total = Snapshot.Bodies.Num() + Snapshot.Constraints.Num()
		+ Snapshot.DisabledCollisionPairs.Num();
	if (Offset > Total)
	{
		return Reject(TEXT("cursor_offset_out_of_range"),
			TEXT("The sealed cursor offset exceeds the exact bounded snapshot."));
	}
	int64 EstimatedBytes = EstimatedBaseReportBytes;
	for (const FHyperAIPhysicsIssue& Issue : Report.Issues)
	{
		EstimatedBytes = SaturatingAdd(EstimatedBytes, EstimateIssueBytes(Issue));
	}
	if (EstimatedBytes > Request.MaxOutputBytes)
	{
		return Reject(TEXT("output_bound_exceeded"),
			TEXT("Capture diagnostics exceed the caller's closed output bound."));
	}
	int32 GlobalIndex = 0;
	int32 Added = 0;
	bool bStopped = false;
	auto Admit = [&](const int64 ItemBytes)
	{
		if (GlobalIndex < Offset)
		{
			++GlobalIndex;
			return false;
		}
		if (Added >= Request.PageSize || EstimatedBytes > Request.MaxOutputBytes - ItemBytes)
		{
			bStopped = true;
			return false;
		}
		EstimatedBytes += ItemBytes;
		++GlobalIndex;
		++Added;
		return true;
	};
	for (const FHyperAIPhysicsBodyRecord& Body : Snapshot.Bodies)
	{
		if (bStopped) break;
		if (Admit(EstimateBodyRecordBytes(Body))) Report.Bodies.Add(Body);
	}
	for (const FHyperAIPhysicsConstraintRecord& Constraint : Snapshot.Constraints)
	{
		if (bStopped) break;
		if (Admit(EstimateConstraintRecordBytes(Constraint))) Report.Constraints.Add(Constraint);
	}
	for (const FHyperAIPhysicsCollisionPairRecord& Pair : Snapshot.DisabledCollisionPairs)
	{
		if (bStopped) break;
		if (Admit(EstimatePairRecordBytes(Pair))) Report.DisabledCollisionPairs.Add(Pair);
	}
	if (bStopped && Added == 0 && Offset < Total)
	{
		return Reject(TEXT("output_bound_exceeded"),
			TEXT("The next exact Physics record cannot fit the closed output budget; no non-advancing cursor is emitted."));
	}
	Report.bTruncated = GlobalIndex < Total;
	if (Report.bTruncated && Report.bCursorEligible)
	{
		Report.NextCursor = BuildCursor(Request.TargetPath,
			Snapshot.Asset.PersistedRevision, Request.PageSize, GlobalIndex);
	}
	Report.bOk = true;
	Report.Status = CaptureStatus;
	Report.Diagnostic = CaptureDiagnostic;
	return Report;
}

FHyperAIPhysicsValidateReport FHyperAIStudioPhysicsContracts::Validate(
	const FHyperAIPhysicsValidateRequest& Request)
{
	FHyperAIPhysicsValidateReport Report;
	Report.Capabilities = GetCapabilityMatrix();
	if (!IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		Report.Status = TEXT("source_candidate_dev_mode_required");
		Report.Diagnostic = TEXT("The exact Physics source cohort is SourceCandidate-only and remains fail-closed outside development mode.");
		return Report;
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath)
		|| (!Request.ExpectedPersistedRevision.IsEmpty()
			&& !IsCanonicalSha256(Request.ExpectedPersistedRevision))
		|| (Request.Policy != TEXT("structural") && Request.Policy != TEXT("simulation_ready"))
		|| Request.MaxIssues < 1 || Request.MaxIssues > MaxIssues
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > MaxReadGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_validation_request");
		Report.Diagnostic = TEXT("Validation requires canonical identity, policy, revision assertion, and closed bounds.");
		return Report;
	}
	FHyperAIStudioPhysicsValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.TargetPath, Request.MaxGameThreadMs, Snapshot,
		CaptureStatus, CaptureDiagnostic))
	{
		Report.Status = CaptureStatus;
		Report.Diagnostic = CaptureDiagnostic;
		return Report;
	}
	Report.bFreshCapture = true;
	if (!Request.ExpectedPersistedRevision.IsEmpty()
		&& Snapshot.Asset.PersistedRevision != Request.ExpectedPersistedRevision)
	{
		Report.Status = Snapshot.Asset.bRevisionComplete
			? TEXT("stale_revision") : TEXT("revision_incomplete");
		Report.Diagnostic = TEXT("The exact fresh capture does not match the asserted complete persisted revision.");
		Report.PersistedRevision = Snapshot.Asset.PersistedRevision;
		return Report;
	}
	ValidateDetached(Snapshot, Request.Policy, Request.MaxIssues,
		Request.MaxOutputBytes, Report);
	Report.bFreshCapture = true;
	return Report;
}

FString FHyperAIStudioPhysicsContracts::ComputePatchSemanticFingerprint(
	const FHyperAIStudioPhysicsPatchPayload& Payload)
{
	using namespace HyperAIStudio::Physics::Private;
	const FHyperAIPhysicsSettingsPatch& Patch = Payload.Patch;
	FString Canonical;
	AppendTokenUnchecked(Canonical, TEXT("hyperai.physics.settings-patch.v1"));
	AppendTokenUnchecked(Canonical, Payload.TargetPath);
	AppendTokenUnchecked(Canonical, Payload.BasePersistedRevision);
	AppendTokenUnchecked(Canonical, Patch.BodyName);
	AppendTokenUnchecked(Canonical, Patch.ExpectedBodyFingerprint);
	AppendTokenUnchecked(Canonical, Patch.bSetCollisionProfile ? TEXT("set_profile") : TEXT("keep_profile"));
	AppendTokenUnchecked(Canonical, Patch.CollisionProfileName);
	AppendTokenUnchecked(Canonical, Patch.bSetCollisionEnabled ? TEXT("set_collision") : TEXT("keep_collision"));
	AppendTokenUnchecked(Canonical, FString::FromInt(Patch.CollisionEnabled));
	AppendTokenUnchecked(Canonical, Patch.bSetSimulatePhysics ? TEXT("set_simulate") : TEXT("keep_simulate"));
	AppendTokenUnchecked(Canonical, Patch.bSimulatePhysics ? TEXT("simulate") : TEXT("not_simulate"));
	AppendTokenUnchecked(Canonical, Patch.bSetSolverSettings ? TEXT("set_solver") : TEXT("keep_solver"));
	AppendTokenUnchecked(Canonical, Patch.ExpectedSolverFingerprint);
	AppendTokenUnchecked(Canonical, FString::FromInt(Patch.PositionIterations));
	AppendTokenUnchecked(Canonical, FString::FromInt(Patch.VelocityIterations));
	AppendTokenUnchecked(Canonical, FString::FromInt(Patch.ProjectionIterations));
	AppendTokenUnchecked(Canonical, CanonicalFloat(Patch.CullDistance));
	AppendTokenUnchecked(Canonical, CanonicalFloat(Patch.MaxDepenetrationVelocity));
	AppendTokenUnchecked(Canonical, CanonicalFloat(Patch.FixedTimeStep));
	AppendTokenUnchecked(Canonical, Patch.bUseLinearJointSolver ? TEXT("linear") : TEXT("nonlinear"));
	AppendTokenUnchecked(Canonical, Patch.bUseManifolds ? TEXT("manifolds") : TEXT("single_contact"));
	AppendTokenUnchecked(Canonical,
		Patch.bSetConstraintCollisionDisabled ? TEXT("set_constraint_collision")
			: TEXT("keep_constraint_collision"));
	AppendTokenUnchecked(Canonical, Patch.ConstraintStableId);
	AppendTokenUnchecked(Canonical, Patch.ExpectedConstraintFingerprint);
	AppendTokenUnchecked(Canonical,
		Patch.bConstraintCollisionDisabled ? TEXT("disabled") : TEXT("enabled"));
	return HashCanonical(Canonical);
}

FHyperAIPhysicsApplyPlanReport FHyperAIStudioPhysicsContracts::BuildPlan(
	const FHyperAIPhysicsApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::Physics::Private;
	FHyperAIPhysicsApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId.Left(FHyperAIStudioDomainLimits::MaxOperationIdChars);
	Report.Capabilities = GetCapabilityMatrix();
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Report.bOk = false;
		Report.bStaged = false;
		Report.bExecutionSubmitted = false;
		Report.bFallbackPermitted = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (!IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		return Reject(TEXT("source_candidate_dev_mode_required"),
			TEXT("The exact Physics source cohort is SourceCandidate-only and remains fail-closed outside development mode."));
	}
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"),
			TEXT("Physics plan preparation requires one bounded exact game-thread capture."));
	}
	if (Request.bDryRun && (!Request.OperationId.IsEmpty()
		|| !Request.ExpectedPlanHash.IsEmpty()))
	{
		return Reject(TEXT("unexpected_submission_fields"),
			TEXT("Dry-run preparation prohibits operation_id and expected_plan_hash."));
	}
	if (!Request.bDryRun
		&& (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId)
			|| !IsCanonicalSha256(Request.ExpectedPlanHash)))
	{
		return Reject(TEXT("invalid_submission_identity"),
			TEXT("Non-dry intent requires one valid operation_id and exact dry-run plan hash."));
	}
	if (!IsCanonicalProjectObjectPath(Request.TargetPath)
		|| !IsCanonicalSha256(Request.ExpectedPersistedRevision)
		|| Request.DeadlineMs < MinPrepareDeadlineMs
		|| Request.DeadlineMs > MaxPrepareDeadlineMs
		|| Request.MaxGameThreadMs < 50
		|| Request.MaxGameThreadMs > MaxMutationGameThreadMs
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > MaxOutputBytes)
	{
		return Reject(TEXT("invalid_plan_bounds_or_identity"),
			TEXT("Plan preparation requires canonical target/CAS identity and closed deadline/work/output bounds."));
	}

	const FHyperAIPhysicsSettingsPatch& Patch = Request.Patch;
	const bool bBodyPatch = Patch.bSetCollisionProfile || Patch.bSetCollisionEnabled
		|| Patch.bSetSimulatePhysics;
	const int32 PatchFieldCount = static_cast<int32>(Patch.bSetCollisionProfile)
		+ static_cast<int32>(Patch.bSetCollisionEnabled)
		+ static_cast<int32>(Patch.bSetSimulatePhysics)
		+ static_cast<int32>(Patch.bSetSolverSettings)
		+ static_cast<int32>(Patch.bSetConstraintCollisionDisabled);
	if (PatchFieldCount < 1)
	{
		return Reject(TEXT("empty_patch"),
			TEXT("The closed Physics settings patch must select at least one non-Epic-equivalent field."));
	}
	if (bBodyPatch && (!IsSafeName(Patch.BodyName)
		|| !IsCanonicalSha256(Patch.ExpectedBodyFingerprint)))
	{
		return Reject(TEXT("invalid_body_patch_identity"),
			TEXT("Body settings require one exact bounded bone name and expected body fingerprint."));
	}
	if (Patch.bSetCollisionProfile && !IsSafeName(Patch.CollisionProfileName))
	{
		return Reject(TEXT("invalid_collision_profile"),
			TEXT("Collision profile must be one bounded simple Unreal profile name."));
	}
	if (Patch.bSetCollisionEnabled
		&& (Patch.CollisionEnabled < 0 || Patch.CollisionEnabled > 5))
	{
		return Reject(TEXT("invalid_collision_enabled"),
			TEXT("collision_enabled must be an exact UE 5.8 ECollisionEnabled integer in [0,5]."));
	}
	if (Patch.bSetSolverSettings
		&& (!IsCanonicalSha256(Patch.ExpectedSolverFingerprint)
			|| Patch.PositionIterations < 1 || Patch.PositionIterations > 50
			|| Patch.VelocityIterations < 0 || Patch.VelocityIterations > 50
			|| Patch.ProjectionIterations < 0 || Patch.ProjectionIterations > 50
			|| !IsFinite(Patch.CullDistance) || Patch.CullDistance < 0.0
			|| !IsFinite(Patch.MaxDepenetrationVelocity)
			|| Patch.MaxDepenetrationVelocity < 0.0
			|| !IsFinite(Patch.FixedTimeStep) || Patch.FixedTimeStep < 0.0
			|| Patch.FixedTimeStep > 1.0))
	{
		return Reject(TEXT("invalid_solver_patch"),
			TEXT("Solver settings or their expected fingerprint are outside the closed finite UE 5.8 envelope."));
	}
	if (Patch.bSetConstraintCollisionDisabled
		&& (!IsCanonicalSha256(Patch.ConstraintStableId)
			|| !IsCanonicalSha256(Patch.ExpectedConstraintFingerprint)))
	{
		return Reject(TEXT("invalid_constraint_patch_identity"),
			TEXT("Constraint collision settings require exact stable-id and current fingerprint assertions."));
	}

	FHyperAIStudioPhysicsValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!CaptureExact(Request.TargetPath,
		FMath::Min(Request.MaxGameThreadMs, MaxReadGameThreadMs), Snapshot,
		CaptureStatus, CaptureDiagnostic))
	{
		return Reject(*CaptureStatus, *CaptureDiagnostic);
	}
	Report.BasePersistedRevision = Snapshot.Asset.PersistedRevision;
	Report.Issues = Snapshot.CaptureIssues;
	if (!Snapshot.bComplete || !Snapshot.Asset.bRevisionComplete
		|| Snapshot.Asset.bPackageDirty || Snapshot.Asset.DiskExistence != TEXT("exists"))
	{
		return Reject(TEXT("persisted_clean_base_required"),
			TEXT("Physics planning requires one complete clean loaded CAS snapshot with proven non-blocking Asset Registry presence."));
	}
	if (Snapshot.Asset.PersistedRevision != Request.ExpectedPersistedRevision)
	{
		return Reject(TEXT("stale_revision"),
			TEXT("The exact loaded PhysicsAsset changed after inspection."));
	}

	bool bNoOp = true;
	if (bBodyPatch)
	{
		const FHyperAIPhysicsBodyRecord* Body = Snapshot.Bodies.FindByPredicate(
			[&](const FHyperAIPhysicsBodyRecord& Candidate)
			{
				return Candidate.BoneName == Patch.BodyName;
			});
		if (!Body || Body->BodyFingerprint != Patch.ExpectedBodyFingerprint)
		{
			return Reject(TEXT("stale_or_missing_body"),
				TEXT("The exact body name/fingerprint assertion no longer matches the loaded snapshot."));
		}
		if (Patch.bSetCollisionProfile)
		{
			bNoOp &= Body->CollisionProfileName == Patch.CollisionProfileName;
		}
		if (Patch.bSetCollisionEnabled)
		{
			bNoOp &= Body->CollisionEnabled == Patch.CollisionEnabled;
		}
		if (Patch.bSetSimulatePhysics)
		{
			bNoOp &= Body->bSimulatePhysics == Patch.bSimulatePhysics;
		}
	}
	if (Patch.bSetSolverSettings)
	{
		if (Snapshot.Asset.Solver.Fingerprint != Patch.ExpectedSolverFingerprint)
		{
			return Reject(TEXT("stale_solver_settings"),
				TEXT("The exact solver fingerprint assertion no longer matches the loaded snapshot."));
		}
		const FHyperAIPhysicsSolverRecord& Solver = Snapshot.Asset.Solver;
		bNoOp &= Solver.PositionIterations == Patch.PositionIterations
			&& Solver.VelocityIterations == Patch.VelocityIterations
			&& Solver.ProjectionIterations == Patch.ProjectionIterations
			&& Solver.CullDistance == Patch.CullDistance
			&& Solver.MaxDepenetrationVelocity == Patch.MaxDepenetrationVelocity
			&& Solver.FixedTimeStep == Patch.FixedTimeStep
			&& Solver.bUseLinearJointSolver == Patch.bUseLinearJointSolver
			&& Solver.bUseManifolds == Patch.bUseManifolds;
	}
	if (Patch.bSetConstraintCollisionDisabled)
	{
		const FHyperAIPhysicsConstraintRecord* Constraint =
			Snapshot.Constraints.FindByPredicate(
				[&](const FHyperAIPhysicsConstraintRecord& Candidate)
				{
					return Candidate.StableId == Patch.ConstraintStableId;
				});
		if (!Constraint
			|| Constraint->ConstraintFingerprint != Patch.ExpectedConstraintFingerprint)
		{
			return Reject(TEXT("stale_or_missing_constraint"),
				TEXT("The exact constraint stable-id/fingerprint assertion no longer matches the loaded snapshot."));
		}
		bNoOp &= Constraint->bDisableCollision == Patch.bConstraintCollisionDisabled;
	}
	if (bNoOp)
	{
		return Reject(TEXT("no_op_patch"),
			TEXT("Every selected closed setting already equals the requested value."));
	}

	const TSharedRef<FHyperAIStudioPhysicsPatchPayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FHyperAIStudioPhysicsPatchPayload, ESPMode::ThreadSafe>();
	Payload->TargetPath = Request.TargetPath;
	Payload->BasePersistedRevision = Snapshot.Asset.PersistedRevision;
	Payload->Patch = Patch;
	Payload->SemanticFingerprint = ComputePatchSemanticFingerprint(*Payload);
	if (!IsCanonicalSha256(Payload->SemanticFingerprint))
	{
		return Reject(TEXT("semantic_fingerprint_failed"),
			TEXT("The closed typed Physics patch could not be sealed."));
	}
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Clone =
		Payload->CloneImmutable();
	if (&Clone.Get() == &Payload.Get() || Clone->GetTypeId() != Payload->GetTypeId()
		|| Clone->GetSchemaFingerprint() != Payload->GetSchemaFingerprint()
		|| Clone->GetSemanticFingerprint() != Payload->GetSemanticFingerprint())
	{
		return Reject(TEXT("immutable_clone_failed"),
			TEXT("Typed execution requires a detached deep immutable payload clone."));
	}

	const FString ProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = GetAdapterDescriptor();
	if (ProjectId.IsEmpty() || !IsCanonicalSha256(Descriptor.AdapterFingerprint))
	{
		return Reject(TEXT("preparation_identity_unavailable"),
			TEXT("Canonical project or exact adapter identity is unavailable."));
	}
	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = PackId;
	Binding.ToolName = TEXT("hyper_physics_apply_plan");
	Binding.VariantId = MutationVariantId;
	Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
	Binding.CanonicalProjectId = ProjectId;
	Binding.ExpectedAdapterFingerprint = Descriptor.AdapterFingerprint;
	Binding.ExpectedAdapterGeneration = 1;
	Binding.ExpectedRegistryEpoch = 1;
	Binding.Prerequisites.PackId = PackId;
	Binding.Prerequisites.bPackEnabled = true;
	Binding.Prerequisites.Revision = 1;
	Binding.Prerequisites.Observations = {
		{TEXT("module.PhysicsAssetEditor"),
			FModuleManager::Get().IsModuleLoaded(TEXT("PhysicsAssetEditor"))
				? EHyperAIStudioDomainPrerequisiteState::Available
				: EHyperAIStudioDomainPrerequisiteState::Missing},
		{TEXT("plugin.ChaosClothAssetEditor"), EHyperAIStudioDomainPrerequisiteState::Missing},
		{LiveProbeId, EHyperAIStudioDomainPrerequisiteState::Available}};
	Binding.Prerequisites.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(
			Binding.Prerequisites);
	Binding.Admission.PackId = PackId;
	Binding.Admission.bPackAdmitted = false;
	Binding.Admission.bEditAdmitted = false;
	Binding.Admission.Revision = 1;
	Binding.Admission.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);

	FHyperAIStudioTypedArtifactContract Contract;
	Contract.Binding = MoveTemp(Binding);
	Contract.ArtifactTypeId = Clone->GetTypeId();
	Contract.ArtifactSchemaFingerprint = Clone->GetSchemaFingerprint();
	Contract.ArtifactSemanticFingerprint = Clone->GetSemanticFingerprint();
	Contract.EffectTarget = Request.TargetPath;
	Contract.DeadlineMs = Request.DeadlineMs;
	Contract.MaxNativeOperations = PatchFieldCount + 4;
	Contract.MaxGameThreadMs = Request.MaxGameThreadMs;
	Contract.MaxOutputBytes = Request.MaxOutputBytes;
	Contract.MaxResultBytes = 256;
	Contract.StageLifetimeMs = StageLifetimeMs;
	Contract.bCompileOnce = true;
	Contract.bSaveOnce = true;
	Contract.bValidateOnce = true;
	Contract.bVerifyFreshOnce = true;
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString PrepareError;
	if (!FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, Prepared, PrepareError))
	{
		return Reject(TEXT("typed_artifact_prepare_failed"), *PrepareError);
	}
	Report.bTypedPrepared = true;
	Report.SemanticFingerprint = Payload->SemanticFingerprint;
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	Report.Effects.TargetCount = 1;
	Report.Effects.PatchFieldCount = PatchFieldCount;
	Report.Effects.bTypedPayloadSealed = true;
	Report.Effects.bDetachedImmutableClone = true;
	Report.Effects.bWouldTransactionOnce = true;
	Report.Effects.bWouldRebuildPhysicsStateOnce = true;
	Report.Effects.bWouldSaveOnce = true;
	Report.Effects.bWouldValidateOnce = true;
	Report.Effects.bWouldFreshVerifyOnce = true;
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("dry_run_valid_execution_blocked");
		Report.Diagnostic = TEXT("Pure public TypedArtifactExecutor::Prepare sealed a detached immutable plan. No transaction, mutation, physics rebuild, compile, simulation, save, stage, submission, or trusted execution occurred.");
		return Report;
	}
	if (Request.ExpectedPlanHash != Prepared.PlanHash)
	{
		return Reject(TEXT("plan_hash_mismatch"),
			TEXT("Non-dry intent does not echo the exact pure dry-run plan hash."));
	}
	return Reject(NonDryCallableState,
		TEXT("No Physics effect ran. A bounded continuation host must transaction once, rebuild/compile or simulate to terminal evidence, save once, independently validate, and fresh-verify exact persisted CAS before submission can exist."));
}

FHyperAIPhysicsInspectReport UHyperAIStudioPhysicsToolset::hyper_physics_inspect(
	const FHyperAIPhysicsInspectRequest& Request)
{
	return FHyperAIStudioPhysicsContracts::Inspect(Request);
}

FHyperAIPhysicsApplyPlanReport UHyperAIStudioPhysicsToolset::hyper_physics_apply_plan(
	const FHyperAIPhysicsApplyPlanRequest& Request)
{
	return FHyperAIStudioPhysicsContracts::BuildPlan(Request);
}

FHyperAIPhysicsValidateReport UHyperAIStudioPhysicsToolset::hyper_physics_validate(
	const FHyperAIPhysicsValidateRequest& Request)
{
	return FHyperAIStudioPhysicsContracts::Validate(Request);
}

FHyperAIStudioPhysicsDomainAdapter::FHyperAIStudioPhysicsDomainAdapter()
	: Descriptor(FHyperAIStudioPhysicsContracts::GetAdapterDescriptor())
{
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioPhysicsDomainAdapter::GetDescriptor() const
{
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioPhysicsDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	auto Reject = [&](const TCHAR* Status, const TCHAR* Diagnostic)
	{
		Result.StatusCode = Status;
		Result.Diagnostic = Diagnostic;
		return Result;
	};
	if (Context.Binding.PackId != Descriptor.PackId
		|| (!Context.Binding.ExpectedAdapterFingerprint.IsEmpty()
			&& Context.Binding.ExpectedAdapterFingerprint != Descriptor.AdapterFingerprint)
		|| Payload.GetBoundedByteSize() <= 0
		|| Payload.GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		return Reject(TEXT("typed_binding_mismatch"),
			TEXT("Physics adapter rejected pack, adapter, or bounded DTO identity."));
	}
	if (Context.Binding.ToolName == TEXT("hyper_physics_inspect")
		&& Context.Binding.VariantId == FHyperAIStudioPhysicsContracts::InspectVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioPhysicsContracts::InspectPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioPhysicsContracts::InspectPayloadSchemaFingerprint())
	{
		const FHyperAIStudioPhysicsInspectPayload& Typed =
			static_cast<const FHyperAIStudioPhysicsInspectPayload&>(Payload);
		const TSharedRef<FHyperAIStudioPhysicsInspectResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioPhysicsInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioPhysicsContracts::Inspect(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(
			FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(
			FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_physics_validate")
		&& Context.Binding.VariantId == FHyperAIStudioPhysicsContracts::ValidateVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Read
		&& Payload.GetTypeId() == FHyperAIStudioPhysicsContracts::ValidatePayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioPhysicsContracts::ValidatePayloadSchemaFingerprint())
	{
		const FHyperAIStudioPhysicsValidatePayload& Typed =
			static_cast<const FHyperAIStudioPhysicsValidatePayload&>(Payload);
		const TSharedRef<FHyperAIStudioPhysicsValidateResultPayload, ESPMode::ThreadSafe> Output =
			MakeShared<FHyperAIStudioPhysicsValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = FHyperAIStudioPhysicsContracts::Validate(Typed.Request);
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Output->Report.Status.Left(
			FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Output->Report.Diagnostic.Left(
			FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	}
	if (Context.Binding.ToolName == TEXT("hyper_physics_apply_plan")
		&& Context.Binding.VariantId == FHyperAIStudioPhysicsContracts::MutationVariantId
		&& Context.Safety == EHyperAIStudioDomainSafety::Edit
		&& Payload.GetTypeId() == FHyperAIStudioPhysicsContracts::PatchPayloadTypeId
		&& Payload.GetSchemaFingerprint()
			== FHyperAIStudioPhysicsContracts::PatchPayloadSchemaFingerprint())
	{
		const FHyperAIStudioPhysicsPatchPayload& Typed =
			static_cast<const FHyperAIStudioPhysicsPatchPayload&>(Payload);
		if (FHyperAIStudioPhysicsContracts::ComputePatchSemanticFingerprint(Typed)
			!= Typed.SemanticFingerprint)
		{
			return Reject(TEXT("typed_payload_drift"),
				TEXT("The detached Physics patch semantic fingerprint drifted."));
		}
		return Reject(FHyperAIStudioPhysicsContracts::NonDryCallableState,
			TEXT("No Physics effect ran. This synchronous adapter cannot transaction, mutate, rebuild/compile, simulate, save, validate, or fresh-verify."));
	}
	return Reject(TEXT("typed_binding_mismatch"),
		TEXT("Physics adapter rejected a non-exact tool, variant, safety, schema, or DTO binding."));
}

void FHyperAIStudioPhysicsRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioPhysicsRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioPhysicsRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	RollBackRegistration();
	bStarted = false;
}

bool FHyperAIStudioPhysicsRegistration::IsRegistered() const
{
	return FHyperAIStudioPhysicsContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled())
		&& bOwnsToolset && AdapterHandle.IsValid() && ProbeHandle.IsValid()
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioPhysicsToolset::StaticClass(),
			FHyperAIStudioPhysicsContracts::GetQualifiedToolsetName());
}

bool FHyperAIStudioPhysicsRegistration::HasLiveOwnership() const
{
	return bOwnsToolset || AdapterHandle.IsValid() || ProbeHandle.IsValid();
}

void FHyperAIStudioPhysicsRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || IsRegistered() || !IsInGameThread() || IsEngineExitRequested()
		|| !UObjectInitialized() || !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioTrustedExecutionFacade::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioPhysicsContracts::IsRegistrationAllowed(
		FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioPhysics, Verbose,
			TEXT("Physics exact source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	// The generated blocking any_of group remains solely core-owned. This module
	// hard-links neither optional editor module and never loads one as a probe.
	if (UPhysicsAsset::StaticClass() == nullptr || IAssetRegistry::Get() == nullptr)
	{
		return;
	}

	Adapter = MakeShared<FHyperAIStudioPhysicsDomainAdapter, ESPMode::ThreadSafe>();
	FString Error;
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(
		Adapter.ToSharedRef(), AdapterHandle, Error))
	{
		UE_LOG(LogHyperAIStudioPhysics, Error,
			TEXT("Physics adapter registration failed closed: %s"), *Error);
		Adapter.Reset();
		return;
	}
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(
		AdapterHandle, FHyperAIStudioPhysicsContracts::LiveProbeId, ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioPhysics, Error,
			TEXT("Physics live-probe registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	FHyperAIStudioTrustedProbeResult Observation;
	Observation.bReady = UPhysicsAsset::StaticClass() != nullptr
		&& IAssetRegistry::Get() != nullptr;
	Observation.StatusCode = Observation.bReady
		? TEXT("ready_loaded_only") : TEXT("required_module_not_loaded");
	Observation.Diagnostic = Observation.bReady
		? TEXT("UPhysicsAsset and the Asset Registry interface are already available; the probe loaded and scanned nothing. Core separately owns the blocking PhysicsAssetEditor/ChaosCloth any_of gate.")
		: TEXT("The loaded-only PhysicsAsset backend interface is unavailable.");
	if (!Observation.bReady
		|| !FHyperAIStudioTrustedExecutionFacade::PublishLiveProbeExact(
			ProbeHandle, Observation, Error))
	{
		if (Error.IsEmpty()) Error = Observation.Diagnostic;
		UE_LOG(LogHyperAIStudioPhysics, Error,
			TEXT("Physics live-probe publication failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
		UHyperAIStudioPhysicsToolset::StaticClass(),
		FHyperAIStudioPhysicsContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioPhysics, Error,
			TEXT("Physics atomic three-tool owner registration failed closed: %s"), *Error);
		RollBackRegistration();
		return;
	}
	bOwnsToolset = true;
}

void FHyperAIStudioPhysicsRegistration::RollBackRegistration()
{
	if (!IsInGameThread())
	{
		return;
	}
	if (bOwnsToolset && UObjectInitialized())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioPhysicsToolset::StaticClass(),
			FHyperAIStudioPhysicsContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioPhysics, Error,
				TEXT("Physics owned-toolset rollback failed closed: %s"), *Error);
			return;
		}
		bOwnsToolset = false;
	}
	if (ProbeHandle.IsValid())
	{
		FString Error;
		if (!FHyperAIStudioTrustedExecutionFacade::UnregisterLiveProbe(ProbeHandle, Error))
		{
			UE_LOG(LogHyperAIStudioPhysics, Error,
				TEXT("Physics probe rollback failed closed: %s"), *Error);
			return;
		}
		ProbeHandle = {};
	}
	if (AdapterHandle.IsValid())
	{
		FString Error;
		const EHyperAIStudioDomainUnregisterResult Outcome =
			FHyperAIStudioTrustedExecutionFacade::UnregisterAdapter(AdapterHandle, Error);
		if (Outcome != EHyperAIStudioDomainUnregisterResult::Removed
			&& Outcome != EHyperAIStudioDomainUnregisterResult::NotFound)
		{
			UE_LOG(LogHyperAIStudioPhysics, Error,
				TEXT("Physics adapter rollback failed closed: %s"), *Error);
			return;
		}
		AdapterHandle = {};
		Adapter.Reset();
	}
}

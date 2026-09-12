// Games by Hyper 2026.

#include "HyperAIStudioAnimationToolset.h"

#include "AnimGraphNode_StateMachineBase.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"
#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "AnimationStateGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationTransitionGraph.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAnimation, Log, All);

namespace HyperAIStudio::Animation::Private
{
	constexpr int32 MinOutputBytes = 4096;
	constexpr double MaxTimelineSeconds = 86400.0;
	constexpr double MaxAbsBlendCoordinate = 1000000000.0;
	constexpr double MaxAbsPlayRate = 1000.0;

	void AppendToken(FString& Out, const FString& Value)
	{
		Out += FString::FromInt(Value.Len());
		Out += TEXT(":");
		Out += Value;
		Out += TEXT("|");
	}

	void AppendInt(FString& Out, const int64 Value)
	{
		AppendToken(Out, FString::Printf(TEXT("%lld"), static_cast<long long>(Value)));
	}

	void AppendBool(FString& Out, const bool bValue)
	{
		AppendToken(Out, bValue ? TEXT("1") : TEXT("0"));
	}

	void AppendDouble(FString& Out, const double Value)
	{
		if (FMath::IsNaN(Value)) AppendToken(Out, TEXT("nan"));
		else if (!FMath::IsFinite(Value)) AppendToken(Out, Value < 0.0 ? TEXT("-inf") : TEXT("+inf"));
		else AppendToken(Out, Value == 0.0 ? TEXT("0") : FString::Printf(TEXT("%.17g"), Value));
	}

	FString ObjectPath(const UObject* Object)
	{
		return IsValid(Object) ? Object->GetPathName() : FString();
	}

	FString Clip(const FString& Value, const int32 Limit = 2048)
	{
		return Value.Left(FMath::Max(0, Limit));
	}

	bool TryGetPrimaryAssetPackageName(const FString& ObjectPath, FName& OutPackageName)
	{
		OutPackageName = NAME_None;
		FString PackageName;
		FString ObjectName;
		if (!FHyperAIStudioAnimationContracts::IsCanonicalProjectObjectPath(ObjectPath)
			|| !ObjectPath.Split(TEXT("."), &PackageName, &ObjectName,
				ESearchCase::CaseSensitive, ESearchDir::FromEnd)
			|| ObjectName != FPackageName::GetShortName(PackageName)
			|| !FPackageName::IsValidLongPackageName(PackageName))
		{
			return false;
		}
		OutPackageName = FName(*PackageName);
		return !OutPackageName.IsNone();
	}

	bool IsNameToken(const FString& Value)
	{
		return !Value.IsEmpty() && Value.Len() <= FHyperAIStudioAnimationContracts::MaxNameCharacters
			&& !Value.Equals(TEXT("None"), ESearchCase::IgnoreCase)
			&& FName::IsValidXName(Value, INVALID_OBJECTNAME_CHARACTERS);
	}

	bool IsDefaultRule(const FHyperAIAnimationTransitionRuleSpec& Value)
	{
		return Value.Kind.IsEmpty() && Value.ParameterName.IsEmpty()
			&& Value.Threshold == -1.0 && Value.bExpectedValue;
	}

	bool IsDefaultNotify(const FHyperAIAnimationNotifySpec& Value)
	{
		return Value.Kind.IsEmpty() && Value.Name.IsEmpty()
			&& Value.TimeSeconds == -1.0 && Value.DurationSeconds == 0.0;
	}

	FString BlueprintStatus(const EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_Unknown: return TEXT("unknown");
		case BS_Dirty: return TEXT("dirty");
		case BS_Error: return TEXT("error");
		case BS_UpToDate: return TEXT("up_to_date");
		case BS_BeingCreated: return TEXT("being_created");
		case BS_UpToDateWithWarnings: return TEXT("up_to_date_with_warnings");
		default: return TEXT("unrecognized");
		}
	}

	FString RecognizedVariant(const UObject* Object)
	{
		if (Cast<UAnimBlueprint>(Object)) return TEXT("animation_blueprint");
		if (Cast<UAnimMontage>(Object)) return TEXT("montage");
		if (Cast<UAnimComposite>(Object)) return TEXT("anim_composite");
		if (Cast<UBlendSpace>(Object)) return TEXT("blend_space");
		if (Cast<UPoseAsset>(Object)) return TEXT("pose_asset");
		if (Cast<UAnimSequence>(Object)) return TEXT("animation_sequence");
		if (Cast<USkeleton>(Object)) return TEXT("skeleton");
		if (Cast<USkeletalMesh>(Object)) return TEXT("skeletal_mesh");
		return FString();
	}

	bool VariantMatches(const FString& Requested, const FString& Actual)
	{
		if (Requested == TEXT("all") || Requested.IsEmpty()) return true;
		if (Requested == TEXT("skeleton_mesh"))
			return Actual == TEXT("skeleton") || Actual == TEXT("skeletal_mesh");
		return Requested == Actual;
	}

	bool AddElement(
		FHyperAIAnimationAssetRecord& Record,
		FHyperAIStudioAnimationValueSnapshot& Snapshot,
		FHyperAIAnimationElementView&& Element)
	{
		if ((Snapshot.WorkDeadlineSeconds > 0.0
			&& FPlatformTime::Seconds() >= Snapshot.WorkDeadlineSeconds)
			|| Record.Elements.Num() >= FHyperAIStudioAnimationContracts::MaxElementsPerAsset)
		{
			Record.bRevisionComplete = false;
			Snapshot.bComplete = false;
			return false;
		}
		Record.Elements.Add(MoveTemp(Element));
		return true;
	}

	void AddIssue(
		TArray<FHyperAIAnimationIssue>& Issues,
		const int32 Limit,
		bool& bTruncated,
		const TCHAR* Code,
		const TCHAR* Severity,
		const FString& Variant,
		const FString& AssetPath,
		const FString& StableId,
		const int32 OperationIndex,
		const FString& Message)
	{
		if (Issues.Num() >= Limit)
		{
			bTruncated = true;
			return;
		}
		FHyperAIAnimationIssue Issue;
		Issue.Code = Code;
		Issue.Severity = Severity;
		Issue.Variant = Variant;
		Issue.AssetPath = AssetPath;
		Issue.StableId = StableId;
		Issue.OperationIndex = OperationIndex;
		Issue.Message = Clip(Message);
		Issues.Add(MoveTemp(Issue));
	}

	bool HasErrors(const TArray<FHyperAIAnimationIssue>& Issues)
	{
		return Issues.ContainsByPredicate([](const FHyperAIAnimationIssue& Issue)
		{
			return Issue.Severity == TEXT("error");
		});
	}

	FString NotifyStableId(
		const FString& AssetPath,
		const FAnimNotifyEvent& Notify,
		const int32 Index)
	{
#if WITH_EDITORONLY_DATA
		if (Notify.Guid.IsValid())
			return TEXT("notify:") + AssetPath + TEXT(":") + Notify.Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
#endif
		return TEXT("notify:") + AssetPath + TEXT(":") + FString::FromInt(Index);
	}

	bool CaptureNotifies(
		const UAnimSequenceBase& Asset,
		FHyperAIAnimationAssetRecord& Record,
		FHyperAIStudioAnimationValueSnapshot& Snapshot)
	{
		Record.NotifyCount = Asset.Notifies.Num();
		for (int32 Index = 0; Index < Asset.Notifies.Num(); ++Index)
		{
			const FAnimNotifyEvent& Notify = Asset.Notifies[Index];
			FHyperAIAnimationElementView Element;
			Element.Kind = TEXT("notify");
			Element.StableId = NotifyStableId(Record.AssetPath, Notify, Index);
			Element.Name = Notify.NotifyName.ToString();
			Element.ClassPath = IsValid(Notify.Notify.Get()) ? Notify.Notify->GetClass()->GetPathName()
				: IsValid(Notify.NotifyStateClass.Get()) ? Notify.NotifyStateClass->GetClass()->GetPathName()
				: FString();
			Element.Index = Index;
			Element.TimeSeconds = Notify.GetTriggerTime();
			Element.DurationSeconds = Notify.GetDuration();
			Element.bFlag = Notify.IsBranchingPoint();
			if (!AddElement(Record, Snapshot, MoveTemp(Element))) return false;
		}
		return true;
	}

	bool CaptureCurves(
		const UAnimSequenceBase& Asset,
		FHyperAIAnimationAssetRecord& Record,
		FHyperAIStudioAnimationValueSnapshot& Snapshot)
	{
		const FRawCurveTracks& Curves = Asset.GetCurveData();
		Record.CurveCount = Curves.FloatCurves.Num() + Curves.VectorCurves.Num()
			+ Curves.TransformCurves.Num();
		auto AddCurve = [&](const FAnimCurveBase& Curve, const TCHAR* Kind, const int32 Index)
		{
			FHyperAIAnimationElementView Element;
			Element.Kind = Kind;
			Element.Name = Curve.GetName().ToString();
			Element.StableId = FString(Kind) + TEXT(":") + Record.AssetPath + TEXT(":") + Element.Name;
			Element.Index = Index;
			return AddElement(Record, Snapshot, MoveTemp(Element));
		};
		for (int32 Index = 0; Index < Curves.FloatCurves.Num(); ++Index)
			if (!AddCurve(Curves.FloatCurves[Index], TEXT("curve.float"), Index)) return false;
		for (int32 Index = 0; Index < Curves.VectorCurves.Num(); ++Index)
			if (!AddCurve(Curves.VectorCurves[Index], TEXT("curve.vector"), Index)) return false;
		for (int32 Index = 0; Index < Curves.TransformCurves.Num(); ++Index)
			if (!AddCurve(Curves.TransformCurves[Index], TEXT("curve.transform"), Index)) return false;
		return true;
	}

	void CaptureAnimBlueprint(
		const UAnimBlueprint& Blueprint,
		FHyperAIAnimationAssetRecord& Record,
		FHyperAIStudioAnimationValueSnapshot& Snapshot)
	{
		Record.bTemplate = Blueprint.bIsTemplate;
		Record.SkeletonPath = ObjectPath(Blueprint.TargetSkeleton);
		// UE 5.8 const GetPreviewMesh() may synchronously resolve a soft reference; loaded-only
		// inspection intentionally omits preview metadata rather than causing disk I/O.
		Record.ParentClassPath = ObjectPath(Blueprint.ParentClass);
		Record.GeneratedClassPath = ObjectPath(Blueprint.GeneratedClass);
		Record.BlueprintStatus = BlueprintStatus(Blueprint.Status);

		TSet<const UEdGraph*> InterfaceLayerGraphs;
		int32 TraversedInterfaceGraphs = 0;
		for (int32 InterfaceIndex = 0; InterfaceIndex < Blueprint.ImplementedInterfaces.Num(); ++InterfaceIndex)
		{
			const FBPInterfaceDescription& Interface = Blueprint.ImplementedInterfaces[InterfaceIndex];
			FHyperAIAnimationElementView Element;
			Element.Kind = TEXT("animation_interface");
			Element.ReferencePath = ObjectPath(Interface.Interface.Get());
			Element.Name = Interface.Interface ? Interface.Interface->GetName() : FString();
			Element.StableId = TEXT("interface:") + Record.AssetPath + TEXT(":") + Element.ReferencePath;
			Element.Index = InterfaceIndex;
			Element.SecondaryIndex = Interface.Graphs.Num();
			if (!AddElement(Record, Snapshot, MoveTemp(Element))) return;
			for (const UEdGraph* InterfaceGraph : Interface.Graphs)
			{
				if (TraversedInterfaceGraphs++
					>= FHyperAIStudioAnimationContracts::MaxElementsPerAsset)
				{
					Record.bRevisionComplete = false;
					Snapshot.bComplete = false;
					return;
				}
				if (IsValid(InterfaceGraph)) InterfaceLayerGraphs.Add(InterfaceGraph);
			}
		}

		TArray<UEdGraph*> Graphs;
		Blueprint.GetAllGraphs(Graphs);
		if (Graphs.Num() > FHyperAIStudioAnimationContracts::MaxElementsPerAsset)
		{
			Graphs.SetNum(FHyperAIStudioAnimationContracts::MaxElementsPerAsset,
				EAllowShrinking::No);
			Record.bRevisionComplete = false;
			Snapshot.bComplete = false;
		}
		TSet<const UEdGraph*> SeenGraphs;
		for (UEdGraph* Graph : Graphs)
		{
			if (!IsValid(Graph) || SeenGraphs.Contains(Graph)) continue;
			SeenGraphs.Add(Graph);
			++Record.GraphCount;
			FString GraphKind = TEXT("animation_graph");
			if (Cast<UAnimationStateMachineGraph>(Graph))
			{
				GraphKind = TEXT("state_machine");
				++Record.StateMachineCount;
			}
			else if (Cast<UAnimationStateGraph>(Graph)) GraphKind = TEXT("state_graph");
			else if (Cast<UAnimationTransitionGraph>(Graph)) GraphKind = TEXT("transition_rule_graph");
			else if (InterfaceLayerGraphs.Contains(Graph)) GraphKind = TEXT("animation_layer_graph");
			else if (Blueprint.FunctionGraphs.Contains(Graph)) GraphKind = TEXT("function_graph");

			FHyperAIAnimationElementView GraphElement;
			GraphElement.Kind = GraphKind;
			GraphElement.Name = Graph->GetName();
			GraphElement.ClassPath = Graph->GetClass()->GetPathName();
			GraphElement.StableId = TEXT("graph:") + Record.AssetPath + TEXT(":") + Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
			GraphElement.SecondaryIndex = Graph->Nodes.Num();
			if (!AddElement(Record, Snapshot, MoveTemp(GraphElement))) return;

			for (int32 NodeIndex = 0; NodeIndex < Graph->Nodes.Num(); ++NodeIndex)
			{
				UEdGraphNode* Node = Graph->Nodes[NodeIndex];
				if (!IsValid(Node))
				{
					Record.bRevisionComplete = false;
					Snapshot.bComplete = false;
					continue;
				}
				FHyperAIAnimationElementView NodeElement;
				NodeElement.Kind = TEXT("graph_node");
				// UObject identity is culture-independent; localized editor presentation never enters CAS.
				NodeElement.Name = Node->GetName();
				NodeElement.ClassPath = Node->GetClass()->GetPathName();
				NodeElement.StableId = TEXT("node:") + Record.AssetPath + TEXT(":")
					+ (Node->NodeGuid.IsValid()
						? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)
						: Graph->GetName() + TEXT(":") + FString::FromInt(NodeIndex));
				NodeElement.Index = NodeIndex;
				if (const UAnimStateNode* State = Cast<UAnimStateNode>(Node))
				{
					NodeElement.Kind = TEXT("state");
					NodeElement.Name = State->GetStateName();
					NodeElement.ContainerName = Graph->GetName();
					NodeElement.bFlag = State->bAlwaysResetOnEntry;
					++Record.StateCount;
				}
				else if (const UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Node))
				{
					NodeElement.Kind = TEXT("transition");
					NodeElement.Name = IsValid(Transition->GetPreviousState())
						? Transition->GetPreviousState()->GetStateName() : FString();
					NodeElement.SecondaryName = IsValid(Transition->GetNextState())
						? Transition->GetNextState()->GetStateName() : FString();
					NodeElement.ContainerName = Graph->GetName();
					NodeElement.DurationSeconds = Transition->CrossfadeDuration;
					NodeElement.bFlag = Transition->bDisabled;
					++Record.TransitionCount;
				}
				else if (const UAnimGraphNode_StateMachineBase* Machine =
					Cast<UAnimGraphNode_StateMachineBase>(Node))
				{
					NodeElement.Kind = TEXT("state_machine_reference");
					NodeElement.Name = const_cast<UAnimGraphNode_StateMachineBase*>(Machine)->GetStateMachineName();
					NodeElement.ReferencePath = ObjectPath(Machine->EditorStateMachineGraph);
				}
				if (!AddElement(Record, Snapshot, MoveTemp(NodeElement))) return;
			}
		}
	}

	void CaptureMontage(
		const UAnimMontage& Montage,
		FHyperAIAnimationAssetRecord& Record,
		FHyperAIStudioAnimationValueSnapshot& Snapshot)
	{
		Record.SkeletonPath = ObjectPath(Montage.GetSkeleton());
		Record.PlayLengthSeconds = Montage.GetPlayLength();
		Record.SampledKeyCount = Montage.GetNumberOfSampledKeys();
		Record.SectionCount = Montage.CompositeSections.Num();
		Record.SlotCount = Montage.SlotAnimTracks.Num();
		for (int32 Index = 0; Index < Montage.CompositeSections.Num(); ++Index)
		{
			const FCompositeSection& Section = Montage.CompositeSections[Index];
			FHyperAIAnimationElementView Element;
			Element.Kind = TEXT("montage_section");
			Element.Name = Section.SectionName.ToString();
			Element.SecondaryName = Section.NextSectionName.ToString();
			Element.StableId = TEXT("section:") + Record.AssetPath + TEXT(":") + Element.Name;
			Element.Index = Index;
			Element.TimeSeconds = Section.GetTime();
			if (!AddElement(Record, Snapshot, MoveTemp(Element))) return;
		}
		for (int32 SlotIndex = 0; SlotIndex < Montage.SlotAnimTracks.Num(); ++SlotIndex)
		{
			const FSlotAnimationTrack& Slot = Montage.SlotAnimTracks[SlotIndex];
			FHyperAIAnimationElementView SlotElement;
			SlotElement.Kind = TEXT("montage_slot");
			SlotElement.Name = Slot.SlotName.ToString();
			SlotElement.StableId = TEXT("slot:") + Record.AssetPath + TEXT(":") + SlotElement.Name;
			SlotElement.Index = SlotIndex;
			SlotElement.SecondaryIndex = Slot.AnimTrack.AnimSegments.Num();
			if (!AddElement(Record, Snapshot, MoveTemp(SlotElement))) return;
			Record.SegmentCount += Slot.AnimTrack.AnimSegments.Num();
			for (int32 SegmentIndex = 0; SegmentIndex < Slot.AnimTrack.AnimSegments.Num(); ++SegmentIndex)
			{
				const FAnimSegment& Segment = Slot.AnimTrack.AnimSegments[SegmentIndex];
				FHyperAIAnimationElementView Element;
				Element.Kind = TEXT("montage_segment");
				Element.StableId = TEXT("segment:") + Record.AssetPath + TEXT(":")
					+ FString::FromInt(SlotIndex) + TEXT(":") + FString::FromInt(SegmentIndex);
				Element.ReferencePath = ObjectPath(Segment.GetAnimReference());
				Element.Index = SlotIndex;
				Element.SecondaryIndex = SegmentIndex;
				Element.TimeSeconds = Segment.StartPos;
				Element.DurationSeconds = Segment.GetLength();
				if (!AddElement(Record, Snapshot, MoveTemp(Element))) return;
			}
		}
		if (!CaptureNotifies(Montage, Record, Snapshot)) return;
		if (!CaptureCurves(Montage, Record, Snapshot)) return;
	}

	void CaptureComposite(
		const UAnimComposite& Composite,
		FHyperAIAnimationAssetRecord& Record,
		FHyperAIStudioAnimationValueSnapshot& Snapshot)
	{
		Record.SkeletonPath = ObjectPath(Composite.GetSkeleton());
		Record.PlayLengthSeconds = Composite.GetPlayLength();
		Record.SampledKeyCount = Composite.GetNumberOfSampledKeys();
		Record.SegmentCount = Composite.AnimationTrack.AnimSegments.Num();
		for (int32 Index = 0; Index < Composite.AnimationTrack.AnimSegments.Num(); ++Index)
		{
			const FAnimSegment& Segment = Composite.AnimationTrack.AnimSegments[Index];
			FHyperAIAnimationElementView Element;
			Element.Kind = TEXT("composite_segment");
			Element.StableId = TEXT("segment:") + Record.AssetPath + TEXT(":") + FString::FromInt(Index);
			Element.ReferencePath = ObjectPath(Segment.GetAnimReference());
			Element.Index = Index;
			Element.TimeSeconds = Segment.StartPos;
			Element.DurationSeconds = Segment.GetLength();
			if (!AddElement(Record, Snapshot, MoveTemp(Element))) return;
		}
		if (!CaptureNotifies(Composite, Record, Snapshot)) return;
		if (!CaptureCurves(Composite, Record, Snapshot)) return;
	}

	void CaptureBlendSpace(
		const UBlendSpace& BlendSpace,
		FHyperAIAnimationAssetRecord& Record,
		FHyperAIStudioAnimationValueSnapshot& Snapshot)
	{
		Record.SkeletonPath = ObjectPath(BlendSpace.GetSkeleton());
		Record.PlayLengthSeconds = BlendSpace.GetPlayLength();
		Record.SampleCount = BlendSpace.GetNumberOfBlendSamples();
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const FBlendParameter& Parameter = BlendSpace.GetBlendParameter(Axis);
			FHyperAIAnimationElementView Element;
			Element.Kind = TEXT("blend_axis");
			Element.Name = Parameter.DisplayName;
			Element.StableId = TEXT("axis:") + Record.AssetPath + TEXT(":") + FString::FromInt(Axis);
			Element.Index = Axis;
			Element.TimeSeconds = Parameter.Min;
			Element.DurationSeconds = Parameter.Max;
			if (!AddElement(Record, Snapshot, MoveTemp(Element))) return;
		}
		const TArray<FBlendSample>& Samples = BlendSpace.GetBlendSamples();
		for (int32 Index = 0; Index < Samples.Num(); ++Index)
		{
			const FBlendSample& Sample = Samples[Index];
			if (Sample.SampleValue.ContainsNaN() || !FMath::IsFinite(Sample.RateScale))
			{
				Record.bRevisionComplete = false;
				Snapshot.bComplete = false;
				return;
			}
			FHyperAIAnimationElementView Element;
			Element.Kind = TEXT("blend_sample");
			Element.ReferencePath = ObjectPath(Sample.Animation);
			Element.Name = FString::Printf(TEXT("%.9g,%.9g,%.9g"),
				Sample.SampleValue.X, Sample.SampleValue.Y, Sample.SampleValue.Z);
			Element.StableId = TEXT("sample:") + Record.AssetPath + TEXT(":") + FString::FromInt(Index);
			Element.Index = Index;
			Element.DurationSeconds = Sample.RateScale;
			Element.bFlag = Sample.bMirror;
			if (!AddElement(Record, Snapshot, MoveTemp(Element))) return;
		}
	}

	void CapturePoseAsset(
		const UPoseAsset& PoseAsset,
		FHyperAIAnimationAssetRecord& Record,
		FHyperAIStudioAnimationValueSnapshot& Snapshot)
	{
		Record.SkeletonPath = ObjectPath(PoseAsset.GetSkeleton());
		const TArray<FName>& Poses = PoseAsset.GetPoseFNames();
		Record.PoseCount = Poses.Num();
		FHyperAIAnimationElementView SourceElement;
		SourceElement.Kind = TEXT("pose_source");
		SourceElement.ReferencePath = ObjectPath(PoseAsset.SourceAnimation);
		// GetBasePoseName() is public but not ENGINE_API in UE 5.8. Use the two
		// equivalent inline accessors so this optional pack never imports an
		// unexported Engine symbol.
		SourceElement.Name = PoseAsset.GetPoseNameByIndex(PoseAsset.GetBasePoseIndex()).ToString();
		SourceElement.StableId = TEXT("pose-source:") + Record.AssetPath;
		if (!AddElement(Record, Snapshot, MoveTemp(SourceElement))) return;
		for (int32 Index = 0; Index < Poses.Num(); ++Index)
		{
			FHyperAIAnimationElementView Element;
			Element.Kind = TEXT("pose");
			Element.Name = Poses[Index].ToString();
			Element.StableId = TEXT("pose:") + Record.AssetPath + TEXT(":") + Element.Name;
			Element.Index = Index;
			if (!AddElement(Record, Snapshot, MoveTemp(Element))) return;
		}
		const TArray<FAnimCurveBase>& Curves = PoseAsset.GetCurveData();
		Record.CurveCount = Curves.Num();
		for (int32 Index = 0; Index < Curves.Num(); ++Index)
		{
			FHyperAIAnimationElementView Element;
			Element.Kind = TEXT("pose_curve");
			Element.Name = Curves[Index].GetName().ToString();
			Element.StableId = TEXT("pose-curve:") + Record.AssetPath + TEXT(":") + Element.Name;
			Element.Index = Index;
			if (!AddElement(Record, Snapshot, MoveTemp(Element))) return;
		}
	}

	void CaptureSequence(
		const UAnimSequence& Sequence,
		FHyperAIAnimationAssetRecord& Record,
		FHyperAIStudioAnimationValueSnapshot& Snapshot)
	{
		Record.SkeletonPath = ObjectPath(Sequence.GetSkeleton());
		Record.PlayLengthSeconds = Sequence.GetPlayLength();
		Record.SampledKeyCount = Sequence.GetNumberOfSampledKeys();
		if (!CaptureNotifies(Sequence, Record, Snapshot)) return;
		if (!CaptureCurves(Sequence, Record, Snapshot)) return;
	}

	void CaptureSkeleton(
		const USkeleton& Skeleton,
		FHyperAIAnimationAssetRecord& Record,
		FHyperAIStudioAnimationValueSnapshot& Snapshot)
	{
		const FReferenceSkeleton& Reference = Skeleton.GetReferenceSkeleton();
		Record.BoneCount = Reference.GetNum();
		for (int32 Index = 0; Index < Reference.GetNum(); ++Index)
		{
			FHyperAIAnimationElementView Element;
			Element.Kind = TEXT("skeleton_bone");
			Element.Name = Reference.GetBoneName(Index).ToString();
			Element.StableId = TEXT("bone:") + Record.AssetPath + TEXT(":") + Element.Name;
			Element.Index = Index;
			Element.SecondaryIndex = Reference.GetParentIndex(Index);
			if (!AddElement(Record, Snapshot, MoveTemp(Element))) return;
		}
		const TArray<TSoftObjectPtr<USkeleton>>& Compatible = Skeleton.GetCompatibleSkeletons();
		for (int32 Index = 0; Index < Compatible.Num(); ++Index)
		{
			FHyperAIAnimationElementView Element;
			Element.Kind = TEXT("compatible_skeleton");
			Element.ReferencePath = Compatible[Index].ToSoftObjectPath().ToString();
			Element.StableId = TEXT("compatibility:") + Record.AssetPath + TEXT(":") + Element.ReferencePath;
			Element.Index = Index;
			if (!AddElement(Record, Snapshot, MoveTemp(Element))) return;
		}
	}

	void CaptureSkeletalMesh(
		const USkeletalMesh& Mesh,
		FHyperAIAnimationAssetRecord& Record,
		FHyperAIStudioAnimationValueSnapshot& Snapshot)
	{
		const USkeleton* Skeleton = Mesh.GetSkeleton();
		Record.SkeletonPath = ObjectPath(Skeleton);
		if (IsValid(Skeleton))
		{
			FHyperAIAnimationElementView Compatibility;
			Compatibility.Kind = TEXT("mesh_skeleton_compatibility");
			Compatibility.StableId = TEXT("mesh-skeleton-compatibility:") + Record.AssetPath;
			Compatibility.ReferencePath = Record.SkeletonPath;
			Compatibility.bFlag = Skeleton->IsCompatibleMesh(&Mesh, true);
			Compatibility.Name = Compatibility.bFlag ? TEXT("compatible") : TEXT("incompatible");
			if (!AddElement(Record, Snapshot, MoveTemp(Compatibility))) return;
		}
		const FReferenceSkeleton& Reference = Mesh.GetRefSkeleton();
		Record.BoneCount = Reference.GetNum();
		for (int32 Index = 0; Index < Reference.GetNum(); ++Index)
		{
			FHyperAIAnimationElementView Element;
			Element.Kind = TEXT("mesh_bone");
			Element.Name = Reference.GetBoneName(Index).ToString();
			Element.StableId = TEXT("bone:") + Record.AssetPath + TEXT(":") + Element.Name;
			Element.Index = Index;
			Element.SecondaryIndex = Reference.GetParentIndex(Index);
			if (!AddElement(Record, Snapshot, MoveTemp(Element))) return;
		}
	}

	bool CaptureObject(
		UObject* Object,
		const FString& RequestedVariant,
		const bool bIncludeDetails,
		FHyperAIStudioAnimationValueSnapshot& Snapshot)
	{
		(void)bIncludeDetails; // Revisions always seal full bounded detail; wrappers trim only after capture.
		if (!IsValid(Object) || Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)) return false;
		const FString Variant = RecognizedVariant(Object);
		if (Variant.IsEmpty() || !VariantMatches(RequestedVariant, Variant)) return false;
		const FString Path = Object->GetPathName();
		if (!FHyperAIStudioAnimationContracts::IsCanonicalProjectObjectPath(Path)) return false;

		FHyperAIAnimationAssetRecord Record;
		Record.Variant = Variant;
		Record.AssetPath = Path;
		Record.StableId = Variant + TEXT(":") + Path;
		Record.bRevisionComplete = true;
		Record.bPackageDirty = Object->GetOutermost() && Object->GetOutermost()->IsDirty();

		if (const UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Object)) CaptureAnimBlueprint(*AnimBlueprint, Record, Snapshot);
		else if (const UAnimMontage* Montage = Cast<UAnimMontage>(Object)) CaptureMontage(*Montage, Record, Snapshot);
		else if (const UAnimComposite* Composite = Cast<UAnimComposite>(Object)) CaptureComposite(*Composite, Record, Snapshot);
		else if (const UBlendSpace* BlendSpace = Cast<UBlendSpace>(Object)) CaptureBlendSpace(*BlendSpace, Record, Snapshot);
		else if (const UPoseAsset* PoseAsset = Cast<UPoseAsset>(Object)) CapturePoseAsset(*PoseAsset, Record, Snapshot);
		else if (const UAnimSequence* Sequence = Cast<UAnimSequence>(Object)) CaptureSequence(*Sequence, Record, Snapshot);
		else if (const USkeleton* Skeleton = Cast<USkeleton>(Object)) CaptureSkeleton(*Skeleton, Record, Snapshot);
		else if (const USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Object)) CaptureSkeletalMesh(*SkeletalMesh, Record, Snapshot);
		else return false;

		Snapshot.Records.Add(MoveTemp(Record));
		return true;
	}

	FString ElementCanonical(const FHyperAIAnimationElementView& Element)
	{
		FString Out;
		AppendToken(Out, Element.Kind);
		AppendToken(Out, Element.StableId);
		AppendToken(Out, Element.Name);
		AppendToken(Out, Element.SecondaryName);
		AppendToken(Out, Element.ContainerName);
		AppendToken(Out, Element.ClassPath);
		AppendToken(Out, Element.ReferencePath);
		AppendInt(Out, Element.Index);
		AppendInt(Out, Element.SecondaryIndex);
		AppendDouble(Out, Element.TimeSeconds);
		AppendDouble(Out, Element.DurationSeconds);
		AppendBool(Out, Element.bFlag);
		return Out;
	}

	FString RecordCanonical(const FHyperAIAnimationAssetRecord& Record)
	{
		FString Out;
		AppendToken(Out, Record.Variant);
		AppendToken(Out, Record.StableId);
		AppendToken(Out, Record.AssetPath);
		AppendBool(Out, Record.bRevisionComplete);
		AppendBool(Out, Record.bPackageDirty);
		AppendBool(Out, Record.bTemplate);
		AppendToken(Out, Record.SkeletonPath);
		AppendToken(Out, Record.PreviewMeshPath);
		AppendToken(Out, Record.ParentClassPath);
		AppendToken(Out, Record.GeneratedClassPath);
		AppendToken(Out, Record.BlueprintStatus);
		AppendDouble(Out, Record.PlayLengthSeconds);
		AppendInt(Out, Record.SampledKeyCount);
		AppendInt(Out, Record.GraphCount);
		AppendInt(Out, Record.StateMachineCount);
		AppendInt(Out, Record.StateCount);
		AppendInt(Out, Record.TransitionCount);
		AppendInt(Out, Record.SectionCount);
		AppendInt(Out, Record.SlotCount);
		AppendInt(Out, Record.SegmentCount);
		AppendInt(Out, Record.NotifyCount);
		AppendInt(Out, Record.CurveCount);
		AppendInt(Out, Record.SampleCount);
		AppendInt(Out, Record.PoseCount);
		AppendInt(Out, Record.BoneCount);
		TArray<FHyperAIAnimationElementView> Sorted = Record.Elements;
		Sorted.Sort([](const auto& A, const auto& B)
		{
			if (A.StableId != B.StableId) return A.StableId < B.StableId;
			return A.Kind < B.Kind;
		});
		for (const auto& Element : Sorted) AppendToken(Out, ElementCanonical(Element));
		return Out;
	}

	FString BackendOperationCanonical(const FHyperAIStudioAnimationBackendOperation& Operation)
	{
		FString Out;
		AppendInt(Out, static_cast<uint8>(Operation.Kind));
		AppendToken(Out, Operation.Type);
		AppendToken(Out, Operation.TargetPath);
		AppendToken(Out, Operation.StableId);
		AppendToken(Out, Operation.ExpectedRevision);
		AppendToken(Out, Operation.ReferencePath);
		AppendToken(Out, Operation.Name);
		AppendToken(Out, Operation.SecondaryName);
		AppendInt(Out, Operation.AxisIndex);
		AppendDouble(Out, Operation.Minimum);
		AppendDouble(Out, Operation.Maximum);
		AppendBool(Out, Operation.bFlag);
		AppendToken(Out, Operation.TransitionRule.Kind);
		AppendToken(Out, Operation.TransitionRule.ParameterName);
		AppendDouble(Out, Operation.TransitionRule.Threshold);
		AppendBool(Out, Operation.TransitionRule.bExpectedValue);
		AppendToken(Out, Operation.Notify.Kind);
		AppendToken(Out, Operation.Notify.Name);
		AppendDouble(Out, Operation.Notify.TimeSeconds);
		AppendDouble(Out, Operation.Notify.DurationSeconds);
		AppendInt(Out, Operation.Segments.Num());
		for (const auto& Segment : Operation.Segments)
		{
			AppendToken(Out, Segment.SequencePath);
			AppendDouble(Out, Segment.StartSeconds);
			AppendDouble(Out, Segment.EndSeconds);
			AppendDouble(Out, Segment.PlayRate);
			AppendInt(Out, Segment.LoopCount);
		}
		AppendInt(Out, Operation.Samples.Num());
		for (const auto& Sample : Operation.Samples)
		{
			AppendToken(Out, Sample.SequencePath);
			AppendDouble(Out, Sample.Position.X);
			AppendDouble(Out, Sample.Position.Y);
			AppendDouble(Out, Sample.Position.Z);
		}
		AppendInt(Out, Operation.Mappings.Num());
		for (const auto& Mapping : Operation.Mappings)
		{
			AppendToken(Out, Mapping.Source);
			AppendToken(Out, Mapping.Target);
		}
		AppendBool(Out, Operation.bCreatesTarget);
		return Out;
	}

	struct FStagingState
	{
		FCriticalSection Mutex;
		TMap<FString, FHyperAIStudioAnimationStagedArtifact> Artifacts;
	};

	FStagingState& GetStagingState()
	{
		static FStagingState State;
		return State;
	}

	int64 NowMonotonicMs()
	{
		return static_cast<int64>(FPlatformTime::Seconds() * 1000.0);
	}

	void PruneExpired(FStagingState& State, const int64 NowMs)
	{
		for (auto It = State.Artifacts.CreateIterator(); It; ++It)
		{
			if (It.Value().ExpiresMonotonicMs <= NowMs) It.RemoveCurrent();
		}
	}

	FString StageKey(const FString& ProjectId, const FString& OperationId)
	{
		return ProjectId + TEXT("\n") + OperationId;
	}
}

const TArray<FHyperAIStudioAnimationVariantDescriptor>&
FHyperAIStudioAnimationFacade::GetVariantDescriptors()
{
	static const TArray<FHyperAIStudioAnimationVariantDescriptor> Descriptors = {
		{TEXT("animation_blueprint"), {},
			{TEXT("Engine"), TEXT("AnimGraph"), TEXT("BlueprintGraph"), TEXT("Kismet"), TEXT("UnrealEd")},
			true, true, false,
			{TEXT("loaded_lifecycle_inspection"), TEXT("state_machine_state_transition_inspection"),
				TEXT("interface_layer_inspection"), TEXT("closed_create_edit_compile_repair_dry_run_stage")},
			{TEXT("ordinary_blueprint_node_pin_property_crud")},
			{TEXT("asset_disk_loading"), TEXT("free_form_transition_expressions"),
				TEXT("preview_soft_reference_reporting"), TEXT("mutation_execution_until_async_host")}},
		{TEXT("montage"), {}, {TEXT("Engine")}, true, true, false,
			{TEXT("loaded_sections_slots_segments_notifies"), TEXT("closed_timeline_dry_run_stage")},
			{TEXT("ordinary_animation_asset_property_crud")},
			{TEXT("notify_class_instantiation"), TEXT("asset_disk_loading"),
				TEXT("preview_soft_reference_reporting"),
				TEXT("mutation_execution_until_async_host")}},
		{TEXT("anim_composite"), {}, {TEXT("Engine")}, true, true, false,
			{TEXT("loaded_segments_notifies_curves"), TEXT("closed_segment_plan_stage")},
			{TEXT("ordinary_animation_asset_property_crud")},
			{TEXT("asset_disk_loading"), TEXT("preview_soft_reference_reporting"),
				TEXT("mutation_execution_until_async_host")}},
		{TEXT("blend_space"), {}, {TEXT("Engine")}, true, true, false,
			{TEXT("loaded_axes_samples"), TEXT("closed_axis_sample_plan_stage")},
			{TEXT("ordinary_animation_asset_property_crud")},
			{TEXT("asset_disk_loading"), TEXT("preview_soft_reference_reporting"),
				TEXT("mutation_execution_until_async_host")}},
		{TEXT("pose_asset"), {}, {TEXT("Engine")}, true, true, false,
			{TEXT("loaded_pose_curve_inspection"), TEXT("closed_source_pose_plan_stage")},
			{TEXT("ordinary_animation_asset_property_crud")},
			{TEXT("asset_disk_loading"), TEXT("preview_soft_reference_reporting"),
				TEXT("mutation_execution_until_async_host")}},
		{TEXT("animation_sequence"), {}, {TEXT("Engine")}, true, true, false,
			{TEXT("loaded_curve_notify_inspection"), TEXT("closed_curve_notify_plan_stage")},
			{TEXT("ordinary_sequence_sampling_and_property_crud")},
			{TEXT("raw_track_bulk_rewrite"), TEXT("asset_disk_loading"),
				TEXT("preview_soft_reference_reporting"),
				TEXT("mutation_execution_until_async_host")}},
		{TEXT("skeleton_mesh"), {}, {TEXT("Engine")}, true, true, false,
			{TEXT("loaded_bone_compatibility_inspection"), TEXT("closed_compatibility_plan_stage")},
			{TEXT("ordinary_skeletal_mesh_and_skeleton_crud")},
			{TEXT("destructive_skeleton_remap"), TEXT("asset_disk_loading"),
				TEXT("preview_soft_reference_reporting"),
				TEXT("mutation_execution_until_async_host")}},
		{TEXT("control_rig"), {TEXT("ControlRig"), TEXT("RigVM")},
			{TEXT("ControlRig"), TEXT("ControlRigDeveloper"), TEXT("ControlRigEditor"), TEXT("RigVM")},
			false, false, false, {},
			{TEXT("AnimationAssistantToolset_control_rig_hierarchy_and_graph_primitives")},
			{TEXT("typed_gap_adapter_not_implemented"), TEXT("reflection_fallback_prohibited")}},
		{TEXT("ik_rig"), {TEXT("IKRig")},
			{TEXT("IKRig"), TEXT("IKRigDeveloper"), TEXT("IKRigEditor")}, false, false, false, {}, {},
			{TEXT("typed_chain_solver_adapter_not_implemented"), TEXT("reflection_fallback_prohibited")}},
		{TEXT("ik_retargeter"), {TEXT("IKRig")},
			{TEXT("IKRig"), TEXT("IKRigDeveloper"), TEXT("IKRigEditor")}, false, false, false, {}, {},
			{TEXT("typed_chain_mapping_adapter_not_implemented"), TEXT("reflection_fallback_prohibited")}},
		{TEXT("pose_search_schema"), {TEXT("PoseSearch")},
			{TEXT("PoseSearch"), TEXT("PoseSearchEditor")}, false, false, false, {}, {},
			{TEXT("typed_schema_channel_adapter_not_implemented"), TEXT("reflection_fallback_prohibited")}},
		{TEXT("pose_search_database"), {TEXT("PoseSearch")},
			{TEXT("PoseSearch"), TEXT("PoseSearchEditor")}, false, false, false, {}, {},
			{TEXT("typed_database_asset_adapter_not_implemented"), TEXT("reflection_fallback_prohibited")}}
	};
	return Descriptors;
}

TArray<FHyperAIAnimationVariantStatus> FHyperAIStudioAnimationFacade::ResolveVariantStatuses()
{
	TArray<FHyperAIAnimationVariantStatus> Result;
	for (const auto& Descriptor : GetVariantDescriptors())
	{
		FHyperAIAnimationVariantStatus Status;
		Status.Variant = Descriptor.Variant;
		Status.RequiredPlugins = Descriptor.RequiredPlugins;
		Status.RequiredModules = Descriptor.RequiredModules;
		Status.bInspectImplemented = Descriptor.bInspectImplemented;
		Status.bPlanSchemaImplemented = Descriptor.bPlanSchemaImplemented;
		Status.bApplyBackendExecutable = Descriptor.bApplyBackendExecutable;
		Status.SupportedCases = Descriptor.SupportedCases;
		Status.DelegatedEpicCases = Descriptor.DelegatedEpicCases;
		Status.UnsupportedCases = Descriptor.UnsupportedCases;
		Status.bPluginsInstalled = true;
		Status.bPluginsEnabled = true;
		for (const FString& PluginId : Descriptor.RequiredPlugins)
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginId);
			Status.bPluginsInstalled &= Plugin.IsValid();
			Status.bPluginsEnabled &= Plugin.IsValid() && Plugin->IsEnabled();
		}
		Status.bModulesLoaded = true;
		for (const FString& ModuleId : Descriptor.RequiredModules)
			Status.bModulesLoaded &= FModuleManager::Get().IsModuleLoaded(*ModuleId);

		if (!Descriptor.bInspectImplemented)
		{
			Status.State = TEXT("adapter_unavailable");
			Status.Remediation = TEXT("The exact optional typed adapter is not implemented; prerequisite observations are informational and no fallback is attempted.");
		}
		else if (!Status.bPluginsInstalled)
		{
			Status.State = TEXT("missing_plugin");
			Status.Remediation = TEXT("Install the exact listed Unreal plugin prerequisites; no fallback is attempted.");
		}
		else if (!Status.bPluginsEnabled)
		{
			Status.State = TEXT("plugin_disabled");
			Status.Remediation = TEXT("Enable the exact listed plugins and restart if Unreal requests it.");
		}
		else if (!Status.bModulesLoaded)
		{
			Status.State = TEXT("enabled_not_loaded");
			Status.Remediation = TEXT("Required modules are not loaded; load only the admitted exact capability module.");
		}
		else if (!Descriptor.bPlanSchemaImplemented)
		{
			Status.State = TEXT("inspect_ready_apply_unavailable");
			Status.Remediation = TEXT("Loaded-only inspection is available; no typed plan schema is exposed.");
		}
		else if (!Descriptor.bApplyBackendExecutable)
		{
			Status.State = TEXT("staged_backend_required");
			Status.Remediation = TEXT("Dry-run and idempotent staging exist; mutation remains blocked until the shared async execution host owns this backend.");
		}
		else Status.State = TEXT("ready");
		Result.Add(MoveTemp(Status));
	}
	return Result;
}

bool FHyperAIStudioAnimationFacade::CaptureLoaded(
	const TArray<FString>& AssetPaths,
	const FString& Variant,
	const bool bIncludeDetails,
	FHyperAIStudioAnimationValueSnapshot& OutSnapshot,
	FString& OutStatus,
	FString& OutDiagnostic,
	const int32 MaxWorkMs)
{
	using namespace HyperAIStudio::Animation::Private;
	OutSnapshot = {};
	OutStatus.Reset();
	OutDiagnostic.Reset();
	if (MaxWorkMs < 1 || MaxWorkMs > 250)
	{
		OutStatus = TEXT("invalid_work_budget");
		OutDiagnostic = TEXT("Loaded-only capture requires a 1..250 ms game-thread budget.");
		return false;
	}
	OutSnapshot.WorkDeadlineSeconds = FPlatformTime::Seconds()
		+ static_cast<double>(MaxWorkMs) / 1000.0;
	if (!IsInGameThread())
	{
		OutStatus = TEXT("game_thread_required");
		OutDiagnostic = TEXT("Loaded UObject inspection is allowed only on Unreal's serialized game thread.");
		return false;
	}
	if (AssetPaths.Num() > FHyperAIStudioAnimationContracts::MaxAssetPaths)
	{
		OutStatus = TEXT("too_many_asset_paths");
		OutDiagnostic = TEXT("Asset path count exceeds the bounded request envelope.");
		return false;
	}
	if (Variant.Len() > FHyperAIStudioAnimationContracts::MaxNameCharacters)
	{
		OutStatus = TEXT("unknown_variant");
		OutDiagnostic = TEXT("Variant exceeds the closed Animation/Rigging name bound.");
		return false;
	}
	for (const FString& Path : AssetPaths)
	{
		if (FPlatformTime::Seconds() >= OutSnapshot.WorkDeadlineSeconds)
		{
			OutStatus = TEXT("capture_budget_exceeded");
			OutDiagnostic = TEXT("Loaded-state path validation exceeded its bounded game-thread budget.");
			return false;
		}
		if (!FHyperAIStudioAnimationContracts::IsCanonicalProjectObjectPath(Path))
		{
			OutStatus = TEXT("invalid_asset_path");
			OutDiagnostic = TEXT("Every path must be one canonical /Game object path.");
			return false;
		}
	}
	const auto& Descriptors = GetVariantDescriptors();
	const bool bKnownVariant = Variant == TEXT("all") || Variant.IsEmpty()
		|| Descriptors.ContainsByPredicate([&](const auto& Entry) { return Entry.Variant == Variant; });
	if (!bKnownVariant)
	{
		OutStatus = TEXT("unknown_variant");
		OutDiagnostic = TEXT("Variant is not in the closed Animation/Rigging prerequisite matrix.");
		return false;
	}
	if (const auto* Descriptor = Descriptors.FindByPredicate(
		[&](const auto& Entry) { return Entry.Variant == Variant; }))
	{
		if (!Descriptor->bInspectImplemented)
		{
			OutStatus = TEXT("variant_adapter_unavailable");
			OutDiagnostic = TEXT("The selected optional variant has no linked typed inspector; no reflection fallback is attempted.");
			return false;
		}
	}

	TArray<FString> SortedPaths = AssetPaths;
	SortedPaths.Sort();
	FString ScopeCanonical;
	AppendToken(ScopeCanonical, TEXT("hyperai.animation.loaded-scope.v1"));
	AppendToken(ScopeCanonical, Variant.IsEmpty() ? TEXT("all") : Variant);
	TSet<FString> RequestedPaths;
	for (const FString& Path : SortedPaths)
	{
		if (FPlatformTime::Seconds() >= OutSnapshot.WorkDeadlineSeconds)
		{
			OutStatus = TEXT("capture_budget_exceeded");
			OutDiagnostic = TEXT("Loaded-state scope validation exceeded its bounded game-thread budget.");
			return false;
		}
		if (RequestedPaths.Contains(Path))
		{
			OutStatus = TEXT("duplicate_asset_path");
			OutDiagnostic = TEXT("Each exact asset path may appear only once in a loaded-state scope.");
			return false;
		}
		RequestedPaths.Add(Path);
		AppendToken(ScopeCanonical, Path);
	}
	OutSnapshot.ScopeFingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(ScopeCanonical);

	TSet<FString> Seen;
	auto AddExact = [&](UObject* Object, const FString& RequestedPath)
	{
		if (!IsValid(Object) || Object->GetPathName() != RequestedPath
			|| Seen.Contains(RequestedPath)) return false;
		const int32 Before = OutSnapshot.Records.Num();
		const bool bAdded = CaptureObject(Object, Variant.IsEmpty() ? TEXT("all") : Variant,
			bIncludeDetails, OutSnapshot);
		if (bAdded && OutSnapshot.Records.Num() > Before) Seen.Add(RequestedPath);
		return bAdded;
	};

	if (!AssetPaths.IsEmpty())
	{
		for (const FString& Path : AssetPaths)
		{
			if (FPlatformTime::Seconds() >= OutSnapshot.WorkDeadlineSeconds)
			{
				OutSnapshot.bComplete = false;
				break;
			}
			++OutSnapshot.LoadedObjectsScanned;
			UObject* Object = FindObject<UObject>(nullptr, *Path);
			if (!AddExact(Object, Path))
			{
				OutSnapshot.bComplete = false;
				bool bIgnored = false;
				AddIssue(OutSnapshot.CaptureIssues, FHyperAIStudioAnimationContracts::MaxIssues,
					bIgnored, TEXT("asset_not_loaded_or_variant_mismatch"), TEXT("error"),
					Variant, Path, TEXT("asset:") + Path, -1,
					TEXT("Exact asset is not already loaded as the requested supported variant; no disk load was attempted."));
			}
		}
	}
	else
	{
		TObjectIterator<UObject> It;
		for (; It
			&& FPlatformTime::Seconds() < OutSnapshot.WorkDeadlineSeconds
			&& OutSnapshot.LoadedObjectsScanned < FHyperAIStudioAnimationContracts::MaxLoadedObjectsScanned
			&& OutSnapshot.Records.Num() < FHyperAIStudioAnimationContracts::MaxRecords; ++It)
		{
			++OutSnapshot.LoadedObjectsScanned;
			UObject* Object = *It;
			if (!IsValid(Object)) continue;
			const FString Path = Object->GetPathName();
			if (!Seen.Contains(Path) && CaptureObject(Object,
				Variant.IsEmpty() ? TEXT("all") : Variant, bIncludeDetails, OutSnapshot))
				Seen.Add(Path);
		}
		if (It)
		{
			OutSnapshot.bComplete = false;
			bool bIgnored = false;
			AddIssue(OutSnapshot.CaptureIssues, FHyperAIStudioAnimationContracts::MaxIssues,
				bIgnored, TEXT("loaded_scope_scan_truncated"), TEXT("error"),
				Variant, FString(), TEXT("loaded-scope"), -1,
				TEXT("Loaded-object scope exceeded its object or record bound; revision completeness is false."));
		}
	}

	FHyperAIStudioAnimationContracts::ComputeSnapshotRevision(OutSnapshot);
	if (!bIncludeDetails)
	{
		for (auto& Record : OutSnapshot.Records)
		{
			Record.bOutputDetailsTruncated = !Record.Elements.IsEmpty();
			Record.Elements.Reset();
		}
	}
	OutStatus = TEXT("captured_loaded_state");
	OutDiagnostic = OutSnapshot.bComplete
		? TEXT("Captured a bounded immutable loaded-state view without disk loading or optional reflection.")
		: TEXT("Captured a partial loaded-state view; revision completeness is false.");
	return true;
}

FString FHyperAIStudioAnimationContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioAnimation.HyperAIStudioAnimationToolset");
}

const TArray<FHyperAIStudioAnimationManifestEntry>& FHyperAIStudioAnimationContracts::GetManifest()
{
	static const TArray<FHyperAIStudioAnimationManifestEntry> Manifest = {
		{TEXT("hyper_animation_inspect"), GetQualifiedToolsetName()},
		{TEXT("hyper_animation_apply_plan"), GetQualifiedToolsetName()},
		{TEXT("hyper_animation_validate"), GetQualifiedToolsetName()}
	};
	return Manifest;
}

bool FHyperAIStudioAnimationContracts::IsPendingTestRegistrationEnabled()
{
	return FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled();
}

bool FHyperAIStudioAnimationContracts::IsRegistrationAllowed(
	const bool bAllowSourceCandidateForDev)
{
	const auto& Manifest = GetManifest();
	if (Manifest.Num() != 3) return false;
	TArray<FString> Names;
	TSet<FString> Unique;
	for (const auto& Entry : Manifest)
	{
		if (Entry.Name.IsEmpty() || Entry.QualifiedToolset != GetQualifiedToolsetName()
			|| Unique.Contains(Entry.Name)) return false;
		Unique.Add(Entry.Name);
		Names.Add(Entry.Name);
	}
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, Names, bAllowSourceCandidateForDev);
}

bool FHyperAIStudioAnimationContracts::IsCanonicalProjectObjectPath(const FString& Path)
{
	if (Path.IsEmpty() || Path.Len() > MaxPathCharacters) return false;
	for (int32 Index = 0; Index < Path.Len(); ++Index)
	{
		if (Path[Index] == TEXT('\0')) return false;
	}
	if (Path.Contains(TEXT("\\")) || Path.Contains(TEXT("..")) || Path.Contains(TEXT(":"))
		|| !Path.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
		|| !FPackageName::IsValidObjectPath(Path)) return false;
	FString PackageName;
	FString ObjectName;
	return Path.Split(TEXT("."), &PackageName, &ObjectName, ESearchCase::CaseSensitive,
		ESearchDir::FromEnd) && !ObjectName.IsEmpty() && !ObjectName.EndsWith(TEXT("_C"));
}

bool FHyperAIStudioAnimationContracts::IsCanonicalSha256(const FString& Value)
{
	if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"), ESearchCase::CaseSensitive)) return false;
	for (int32 Index = 7; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!((Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f')))) return false;
	}
	return true;
}

FString FHyperAIStudioAnimationContracts::PayloadSchemaFingerprint()
{
	static const FString Fingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("animation.payload.v1|closed_base_animation_operations|typed_kind_binding|stable_element_selectors|same_plan_create_lifecycle|loaded_only_cas|edit|no_client_authorization|no_script_or_class_instantiation"));
	return Fingerprint;
}

FString FHyperAIStudioAnimationContracts::ResultSchemaFingerprint()
{
	static const FString Fingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("animation.result.v1|phase|revision|valid|error_count|bounded"));
	return Fingerprint;
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioAnimationContracts::GetBaseAdapterDescriptor()
{
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = PackId;
		Value.AdapterId = TEXT("adapter.animation.base_animation_assets");
		Value.SemanticVersion = TEXT("1.0.0");
		Value.AdapterVersion = 1;
		Value.Variants.Add({TEXT("hyper_animation_apply_plan"), MutationVariantId,
			PayloadTypeId, PayloadSchemaFingerprint(), ResultTypeId, ResultSchemaFingerprint(),
			EHyperAIStudioDomainSafety::Edit});
		Value.ContractFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint =
			FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

bool FHyperAIStudioAnimationContracts::ValidateOperationShape(
	const FHyperAIAnimationPlanOperation& Operation,
	FHyperAIStudioAnimationBackendOperation& OutOperation,
	FString& OutErrorCode,
	FString& OutError)
{
	using namespace HyperAIStudio::Animation::Private;
	OutOperation = {};
	OutErrorCode.Reset();
	OutError.Reset();
	auto Fail = [&](const TCHAR* Code, const TCHAR* Message)
	{
		OutErrorCode = Code;
		OutError = Message;
		return false;
	};
	if (!IsCanonicalProjectObjectPath(Operation.TargetPath))
		return Fail(TEXT("invalid_target_path"), TEXT("Target must be one canonical /Game object path."));
	if (Operation.Type.IsEmpty() || Operation.Type.Len() > MaxNameCharacters
		|| Operation.StableId.Len() > MaxStableIdCharacters
		|| Operation.Name.Len() > MaxNameCharacters
		|| Operation.SecondaryName.Len() > MaxNameCharacters
		|| Operation.ReferencePath.Len() > MaxPathCharacters
		|| Operation.Segments.Num() > MaxNestedItems || Operation.Samples.Num() > MaxNestedItems
		|| Operation.Mappings.Num() > MaxNestedItems)
		return Fail(TEXT("field_too_long"), TEXT("Operation field or nested array exceeds its closed bound."));
	if (Operation.StableId.Contains(TEXT("\r")) || Operation.StableId.Contains(TEXT("\n")))
		return Fail(TEXT("invalid_stable_id"), TEXT("Stable element identity cannot contain control-line delimiters."));
	const bool bUsesStableId = Operation.Type == TEXT("montage.remove_notify")
		|| Operation.Type == TEXT("sequence.remove_notify");
	if (!bUsesStableId && !Operation.StableId.IsEmpty())
		return Fail(TEXT("unexpected_stable_id"), TEXT("This operation variant does not accept a stable element selector."));
	if (!FMath::IsFinite(Operation.Minimum) || !FMath::IsFinite(Operation.Maximum)
		|| !FMath::IsFinite(Operation.TransitionRule.Threshold)
		|| !FMath::IsFinite(Operation.Notify.TimeSeconds)
		|| !FMath::IsFinite(Operation.Notify.DurationSeconds))
		return Fail(TEXT("non_finite_number"), TEXT("Operation contains a non-finite numeric value."));

	const bool bCreate = Operation.Type.EndsWith(TEXT(".create"), ESearchCase::CaseSensitive);
	FName CreatePackageName;
	if (bCreate && !TryGetPrimaryAssetPackageName(Operation.TargetPath, CreatePackageName))
		return Fail(TEXT("invalid_create_target_name"),
			TEXT("Create target must use the canonical primary asset name matching its package short name."));
	if ((bCreate && !Operation.ExpectedRevision.IsEmpty())
		|| (!bCreate && !Operation.ExpectedRevision.IsEmpty()
			&& !IsCanonicalSha256(Operation.ExpectedRevision)))
		return Fail(TEXT("revision_shape_invalid"),
			TEXT("Create operations require an empty revision; edits require either one canonical target revision or an empty revision when they follow a same-plan create."));
	if (!Operation.ReferencePath.IsEmpty() && !IsCanonicalProjectObjectPath(Operation.ReferencePath))
		return Fail(TEXT("invalid_reference_path"), TEXT("Reference must be empty or one canonical /Game object path."));

	OutOperation.Type = Operation.Type;
	OutOperation.TargetPath = Operation.TargetPath;
	OutOperation.StableId = Operation.StableId;
	OutOperation.ExpectedRevision = Operation.ExpectedRevision;
	OutOperation.ReferencePath = Operation.ReferencePath;
	OutOperation.Name = Operation.Name;
	OutOperation.SecondaryName = Operation.SecondaryName;
	OutOperation.AxisIndex = Operation.AxisIndex;
	OutOperation.Minimum = Operation.Minimum;
	OutOperation.Maximum = Operation.Maximum;
	OutOperation.bFlag = Operation.bFlag;
	OutOperation.TransitionRule = Operation.TransitionRule;
	OutOperation.Notify = Operation.Notify;
	OutOperation.Segments = Operation.Segments;
	OutOperation.Samples = Operation.Samples;
	OutOperation.Mappings = Operation.Mappings;
	OutOperation.bCreatesTarget = bCreate;

	auto NoReference = [&] { return Operation.ReferencePath.IsEmpty(); };
	auto NoNames = [&] { return Operation.Name.IsEmpty() && Operation.SecondaryName.IsEmpty(); };
	auto NoAxis = [&] { return Operation.AxisIndex == -1 && Operation.Minimum == 0.0
		&& Operation.Maximum == 0.0 && !Operation.bFlag; };
	auto NoCollections = [&] { return Operation.Segments.IsEmpty() && Operation.Samples.IsEmpty()
		&& Operation.Mappings.IsEmpty(); };
	auto BaseEmpty = [&] { return NoReference() && NoNames() && NoAxis()
		&& IsDefaultRule(Operation.TransitionRule) && IsDefaultNotify(Operation.Notify)
		&& NoCollections(); };
	auto ValidNotify = [&]
	{
		return Operation.Notify.Kind == TEXT("named_event")
			&& IsNameToken(Operation.Notify.Name)
			&& Operation.Notify.TimeSeconds >= 0.0
			&& Operation.Notify.TimeSeconds <= MaxTimelineSeconds
			&& Operation.Notify.DurationSeconds >= 0.0
			&& Operation.Notify.DurationSeconds <= MaxTimelineSeconds;
	};
	auto ValidTargetNotifyStableId = [&]
	{
		return Operation.StableId.StartsWith(
			TEXT("notify:") + Operation.TargetPath + TEXT(":"), ESearchCase::CaseSensitive)
			&& Operation.StableId.Len() > Operation.TargetPath.Len() + 8;
	};
	auto ValidTransitionEndpoints = [&]
	{
		return Operation.Mappings.Num() == 1
			&& IsNameToken(Operation.Mappings[0].Source)
			&& IsNameToken(Operation.Mappings[0].Target)
			&& Operation.Mappings[0].Source != Operation.Mappings[0].Target;
	};
	auto RequireSkeletonReference = [&]
	{
		return !Operation.ReferencePath.IsEmpty() && NoNames() && NoAxis()
			&& IsDefaultRule(Operation.TransitionRule) && IsDefaultNotify(Operation.Notify)
			&& NoCollections();
	};

	if (Operation.Type == TEXT("animbp.create"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::AnimBlueprintCreate;
		if (!RequireSkeletonReference()) return Fail(TEXT("invalid_animbp_create_shape"),
			TEXT("animbp.create accepts only a target and one loaded Skeleton reference."));
	}
	else if (Operation.Type == TEXT("animbp.set_skeleton"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::AnimBlueprintSetSkeleton;
		if (!RequireSkeletonReference()) return Fail(TEXT("invalid_animbp_skeleton_shape"),
			TEXT("animbp.set_skeleton accepts only target, CAS, and a loaded Skeleton reference."));
	}
	else if (Operation.Type == TEXT("animbp.add_state_machine"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::AnimBlueprintAddStateMachine;
		if (!NoReference() || !IsNameToken(Operation.Name) || !Operation.SecondaryName.IsEmpty()
			|| !NoAxis() || !IsDefaultRule(Operation.TransitionRule)
			|| !IsDefaultNotify(Operation.Notify) || !NoCollections())
			return Fail(TEXT("invalid_state_machine_shape"), TEXT("State-machine creation requires exactly one valid name."));
	}
	else if (Operation.Type == TEXT("animbp.add_state"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::AnimBlueprintAddState;
		if (!NoReference() || !IsNameToken(Operation.Name) || !IsNameToken(Operation.SecondaryName)
			|| !NoAxis() || !IsDefaultRule(Operation.TransitionRule)
			|| !IsDefaultNotify(Operation.Notify) || !NoCollections())
			return Fail(TEXT("invalid_state_shape"), TEXT("State creation requires state-machine name and new state name."));
	}
	else if (Operation.Type == TEXT("animbp.add_transition"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::AnimBlueprintAddTransition;
		if (!NoReference() || !IsNameToken(Operation.Name) || !Operation.SecondaryName.IsEmpty()
			|| Operation.AxisIndex != -1
			|| Operation.Minimum != 0.0 || Operation.Maximum != 0.0
			|| !IsDefaultRule(Operation.TransitionRule) || !IsDefaultNotify(Operation.Notify)
			|| !Operation.Segments.IsEmpty() || !Operation.Samples.IsEmpty()
			|| !ValidTransitionEndpoints())
			return Fail(TEXT("invalid_transition_shape"),
				TEXT("Transition creation requires a machine name, one distinct source-to-target mapping, and optional bidirectional flag."));
	}
	else if (Operation.Type == TEXT("animbp.set_transition_rule"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::AnimBlueprintSetTransitionRule;
		const bool bBool = Operation.TransitionRule.Kind == TEXT("bool_parameter")
			&& IsNameToken(Operation.TransitionRule.ParameterName)
			&& Operation.TransitionRule.Threshold == -1.0;
		const bool bRatio = Operation.TransitionRule.Kind == TEXT("time_remaining_ratio")
			&& Operation.TransitionRule.ParameterName.IsEmpty()
			&& Operation.TransitionRule.Threshold >= 0.0
			&& Operation.TransitionRule.Threshold <= 1.0
			&& Operation.TransitionRule.bExpectedValue;
		const bool bAutomatic = Operation.TransitionRule.Kind == TEXT("automatic_sequence_end")
			&& Operation.TransitionRule.ParameterName.IsEmpty()
			&& Operation.TransitionRule.Threshold == -1.0
			&& Operation.TransitionRule.bExpectedValue;
		if (!NoReference() || !IsNameToken(Operation.Name) || !Operation.SecondaryName.IsEmpty()
			|| !NoAxis() || !(bBool || bRatio || bAutomatic)
			|| !IsDefaultNotify(Operation.Notify)
			|| !Operation.Segments.IsEmpty() || !Operation.Samples.IsEmpty()
			|| !ValidTransitionEndpoints())
			return Fail(TEXT("invalid_transition_rule_shape"),
				TEXT("Transition rules require a machine name, one source-to-target mapping, and only the closed bool, time-ratio, or automatic vocabulary."));
	}
	else if (Operation.Type == TEXT("animbp.add_interface"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::AnimBlueprintAddInterface;
		if (Operation.ReferencePath.IsEmpty() || !NoNames() || !NoAxis()
			|| !IsDefaultRule(Operation.TransitionRule) || !IsDefaultNotify(Operation.Notify)
			|| !NoCollections())
			return Fail(TEXT("invalid_interface_shape"), TEXT("Interface addition requires exactly one loaded interface asset reference."));
	}
	else if (Operation.Type == TEXT("animbp.add_layer"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::AnimBlueprintAddLayer;
		if (!NoReference() || !IsNameToken(Operation.Name) || !Operation.SecondaryName.IsEmpty()
			|| !NoAxis() || !IsDefaultRule(Operation.TransitionRule)
			|| !IsDefaultNotify(Operation.Notify) || !NoCollections())
			return Fail(TEXT("invalid_layer_shape"), TEXT("Layer addition requires exactly one valid layer name."));
	}
	else if (Operation.Type == TEXT("animbp.compile_repair"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::AnimBlueprintCompileRepair;
		if (!BaseEmpty()) return Fail(TEXT("invalid_compile_repair_shape"),
			TEXT("Compile/repair accepts only target and exact revision."));
	}
	else if (Operation.Type == TEXT("montage.create"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::MontageCreate;
		if (!RequireSkeletonReference()) return Fail(TEXT("invalid_montage_create_shape"),
			TEXT("montage.create accepts only a target and loaded Skeleton reference."));
	}
	else if (Operation.Type == TEXT("montage.add_section"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::MontageAddSection;
		if (!NoReference() || !IsNameToken(Operation.Name) || !Operation.SecondaryName.IsEmpty()
			|| Operation.AxisIndex != -1 || Operation.Minimum < 0.0
			|| Operation.Minimum > MaxTimelineSeconds || Operation.Maximum != 0.0
			|| Operation.bFlag || !IsDefaultRule(Operation.TransitionRule)
			|| !IsDefaultNotify(Operation.Notify) || !NoCollections())
			return Fail(TEXT("invalid_section_shape"), TEXT("Section addition requires one name and non-negative start time in minimum."));
	}
	else if (Operation.Type == TEXT("montage.remove_section"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::MontageRemoveSection;
		if (!NoReference() || !IsNameToken(Operation.Name) || !Operation.SecondaryName.IsEmpty()
			|| !NoAxis() || !IsDefaultRule(Operation.TransitionRule)
			|| !IsDefaultNotify(Operation.Notify) || !NoCollections())
			return Fail(TEXT("invalid_remove_section_shape"), TEXT("Section removal requires exactly one section name."));
	}
	else if (Operation.Type == TEXT("montage.link_sections"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::MontageLinkSections;
		if (!NoReference() || !IsNameToken(Operation.Name)
			|| (!Operation.SecondaryName.IsEmpty() && !IsNameToken(Operation.SecondaryName))
			|| !NoAxis() || !IsDefaultRule(Operation.TransitionRule)
			|| !IsDefaultNotify(Operation.Notify) || !NoCollections())
			return Fail(TEXT("invalid_section_link_shape"), TEXT("Section link requires a source and an optional next-section name."));
	}
	else if (Operation.Type == TEXT("montage.add_notify"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::MontageAddNotify;
		if (!NoReference() || !NoNames() || !NoAxis() || !IsDefaultRule(Operation.TransitionRule)
			|| !ValidNotify() || !NoCollections())
			return Fail(TEXT("invalid_montage_notify_shape"), TEXT("Montage notify uses one bounded named_event spec only."));
	}
	else if (Operation.Type == TEXT("montage.remove_notify"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::MontageRemoveNotify;
		if (!NoReference() || !NoNames() || !ValidTargetNotifyStableId()
			|| !NoAxis() || !IsDefaultRule(Operation.TransitionRule)
			|| !IsDefaultNotify(Operation.Notify) || !NoCollections())
			return Fail(TEXT("invalid_remove_notify_shape"), TEXT("Notify removal requires the exact stable notify id emitted for this target."));
	}
	else if (Operation.Type == TEXT("composite.create"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::CompositeCreate;
		if (!RequireSkeletonReference()) return Fail(TEXT("invalid_composite_create_shape"),
			TEXT("composite.create accepts only a target and loaded Skeleton reference."));
	}
	else if (Operation.Type == TEXT("composite.set_segments"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::CompositeSetSegments;
		if (!NoReference() || !NoNames() || !NoAxis() || !IsDefaultRule(Operation.TransitionRule)
			|| !IsDefaultNotify(Operation.Notify) || Operation.Segments.IsEmpty()
			|| !Operation.Samples.IsEmpty() || !Operation.Mappings.IsEmpty())
			return Fail(TEXT("invalid_composite_segments_shape"), TEXT("Composite replacement requires only a non-empty bounded segment list."));
		for (const auto& Segment : Operation.Segments)
		{
			if (!IsCanonicalProjectObjectPath(Segment.SequencePath)
				|| !FMath::IsFinite(Segment.StartSeconds) || !FMath::IsFinite(Segment.EndSeconds)
				|| !FMath::IsFinite(Segment.PlayRate) || Segment.StartSeconds < 0.0
				|| Segment.EndSeconds <= Segment.StartSeconds
				|| Segment.EndSeconds > MaxTimelineSeconds || FMath::IsNearlyZero(Segment.PlayRate)
				|| FMath::Abs(Segment.PlayRate) > MaxAbsPlayRate
				|| Segment.LoopCount < 1 || Segment.LoopCount > 1024)
				return Fail(TEXT("invalid_segment"), TEXT("Every segment needs a canonical loaded sequence, finite range/rate, and bounded loop count."));
		}
	}
	else if (Operation.Type == TEXT("blend_space.create"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::BlendSpaceCreate;
		if (!RequireSkeletonReference()) return Fail(TEXT("invalid_blend_space_create_shape"),
			TEXT("blend_space.create accepts only a target and loaded Skeleton reference."));
	}
	else if (Operation.Type == TEXT("blend_space.set_axis"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::BlendSpaceSetAxis;
		if (!NoReference() || !IsNameToken(Operation.Name) || !Operation.SecondaryName.IsEmpty()
			|| Operation.AxisIndex < 0 || Operation.AxisIndex > 2
			|| Operation.Maximum <= Operation.Minimum
			|| FMath::Abs(Operation.Minimum) > MaxAbsBlendCoordinate
			|| FMath::Abs(Operation.Maximum) > MaxAbsBlendCoordinate || Operation.bFlag
			|| !IsDefaultRule(Operation.TransitionRule) || !IsDefaultNotify(Operation.Notify)
			|| !NoCollections())
			return Fail(TEXT("invalid_blend_axis_shape"), TEXT("Blend axis requires index 0..2, name, and an increasing finite range."));
	}
	else if (Operation.Type == TEXT("blend_space.set_samples"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::BlendSpaceSetSamples;
		if (!NoReference() || !NoNames() || !NoAxis() || !IsDefaultRule(Operation.TransitionRule)
			|| !IsDefaultNotify(Operation.Notify) || Operation.Samples.IsEmpty()
			|| !Operation.Segments.IsEmpty() || !Operation.Mappings.IsEmpty())
			return Fail(TEXT("invalid_blend_samples_shape"), TEXT("Blend-space replacement requires only a non-empty bounded sample list."));
		TSet<FString> Positions;
		for (const auto& Sample : Operation.Samples)
		{
			if (!IsCanonicalProjectObjectPath(Sample.SequencePath) || Sample.Position.ContainsNaN()
				|| FMath::Abs(Sample.Position.X) > MaxAbsBlendCoordinate
				|| FMath::Abs(Sample.Position.Y) > MaxAbsBlendCoordinate
				|| FMath::Abs(Sample.Position.Z) > MaxAbsBlendCoordinate)
				return Fail(TEXT("invalid_blend_sample"), TEXT("Every sample needs a canonical loaded sequence and finite position."));
			const FString Position = FString::Printf(TEXT("%.17g|%.17g|%.17g"), Sample.Position.X, Sample.Position.Y, Sample.Position.Z);
			if (Positions.Contains(Position)) return Fail(TEXT("duplicate_blend_position"), TEXT("Blend-space sample positions must be unique."));
			Positions.Add(Position);
		}
	}
	else if (Operation.Type == TEXT("pose_asset.create"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::PoseAssetCreate;
		if (!RequireSkeletonReference()) return Fail(TEXT("invalid_pose_asset_create_shape"),
			TEXT("pose_asset.create accepts only a target and loaded Skeleton reference."));
	}
	else if (Operation.Type == TEXT("pose_asset.set_source"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::PoseAssetSetSource;
		if (Operation.ReferencePath.IsEmpty() || !NoNames() || !NoAxis()
			|| !IsDefaultRule(Operation.TransitionRule) || !IsDefaultNotify(Operation.Notify)
			|| !NoCollections())
			return Fail(TEXT("invalid_pose_source_shape"), TEXT("Pose source requires exactly one loaded Animation Sequence reference."));
	}
	else if (Operation.Type == TEXT("pose_asset.set_pose_names"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::PoseAssetSetPoseNames;
		if (!NoReference() || !NoNames() || !NoAxis() || !IsDefaultRule(Operation.TransitionRule)
			|| !IsDefaultNotify(Operation.Notify) || Operation.Mappings.IsEmpty()
			|| !Operation.Segments.IsEmpty() || !Operation.Samples.IsEmpty())
			return Fail(TEXT("invalid_pose_names_shape"), TEXT("Pose renaming requires only a non-empty source-to-target mapping list."));
		TSet<FString> Sources;
		TSet<FString> Targets;
		for (const auto& Mapping : Operation.Mappings)
		{
			if (!IsNameToken(Mapping.Source) || !IsNameToken(Mapping.Target)
				|| Sources.Contains(Mapping.Source) || Targets.Contains(Mapping.Target))
				return Fail(TEXT("invalid_pose_name_mapping"), TEXT("Pose mappings require unique valid source and target names."));
			Sources.Add(Mapping.Source);
			Targets.Add(Mapping.Target);
		}
	}
	else if (Operation.Type == TEXT("sequence.add_curve") || Operation.Type == TEXT("sequence.remove_curve"))
	{
		OutOperation.Kind = Operation.Type == TEXT("sequence.add_curve")
			? EHyperAIStudioAnimationOperationKind::SequenceAddCurve
			: EHyperAIStudioAnimationOperationKind::SequenceRemoveCurve;
		if (!NoReference() || !IsNameToken(Operation.Name) || !Operation.SecondaryName.IsEmpty()
			|| !NoAxis() || !IsDefaultRule(Operation.TransitionRule)
			|| !IsDefaultNotify(Operation.Notify) || !NoCollections())
			return Fail(TEXT("invalid_sequence_curve_shape"), TEXT("Sequence curve operation requires exactly one curve name."));
	}
	else if (Operation.Type == TEXT("sequence.add_notify"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::SequenceAddNotify;
		if (!NoReference() || !NoNames() || !NoAxis() || !IsDefaultRule(Operation.TransitionRule)
			|| !ValidNotify() || !NoCollections())
			return Fail(TEXT("invalid_sequence_notify_shape"), TEXT("Sequence notify uses one bounded named_event spec only."));
	}
	else if (Operation.Type == TEXT("sequence.remove_notify"))
	{
		OutOperation.Kind = EHyperAIStudioAnimationOperationKind::SequenceRemoveNotify;
		if (!NoReference() || !NoNames() || !ValidTargetNotifyStableId()
			|| !NoAxis() || !IsDefaultRule(Operation.TransitionRule)
			|| !IsDefaultNotify(Operation.Notify) || !NoCollections())
			return Fail(TEXT("invalid_sequence_remove_notify_shape"), TEXT("Sequence notify removal requires the exact stable notify id emitted for this target."));
	}
	else if (Operation.Type == TEXT("skeleton.add_compatible")
		|| Operation.Type == TEXT("skeleton.remove_compatible"))
	{
		OutOperation.Kind = Operation.Type == TEXT("skeleton.add_compatible")
			? EHyperAIStudioAnimationOperationKind::SkeletonAddCompatible
			: EHyperAIStudioAnimationOperationKind::SkeletonRemoveCompatible;
		if (Operation.ReferencePath.IsEmpty() || Operation.ReferencePath == Operation.TargetPath
			|| !NoNames() || !NoAxis() || !IsDefaultRule(Operation.TransitionRule)
			|| !IsDefaultNotify(Operation.Notify) || !NoCollections())
			return Fail(TEXT("invalid_skeleton_compatibility_shape"),
				TEXT("Compatibility operation requires one distinct loaded Skeleton reference."));
	}
	else if (Operation.Type.StartsWith(TEXT("control_rig."))
		|| Operation.Type.StartsWith(TEXT("rigvm."))
		|| Operation.Type.StartsWith(TEXT("ik_rig."))
		|| Operation.Type.StartsWith(TEXT("ik_retargeter."))
		|| Operation.Type.StartsWith(TEXT("pose_search.")))
	{
		return Fail(TEXT("variant_adapter_unavailable"),
			TEXT("Operation belongs to an optional typed variant that is not linked; no reflection or script fallback is attempted."));
	}
	else
	{
		return Fail(TEXT("unknown_operation_type"),
			TEXT("Operation type is not in the closed Animation/Rigging edit schema."));
	}
	return true;
}

FString FHyperAIStudioAnimationContracts::ComputeSnapshotRevision(
	FHyperAIStudioAnimationValueSnapshot& Snapshot)
{
	using namespace HyperAIStudio::Animation::Private;
	Snapshot.Records.Sort([](const auto& A, const auto& B) { return A.StableId < B.StableId; });
	for (auto& Record : Snapshot.Records)
	{
		if (Snapshot.WorkDeadlineSeconds > 0.0
			&& FPlatformTime::Seconds() >= Snapshot.WorkDeadlineSeconds)
		{
			Snapshot.bComplete = false;
			break;
		}
		Record.Elements.Sort([](const auto& A, const auto& B)
		{
			if (A.StableId != B.StableId) return A.StableId < B.StableId;
			return A.Kind < B.Kind;
		});
		if (!FMath::IsFinite(Record.PlayLengthSeconds)
			|| Record.Elements.ContainsByPredicate([](const FHyperAIAnimationElementView& Element)
			{
				return !FMath::IsFinite(Element.TimeSeconds)
					|| !FMath::IsFinite(Element.DurationSeconds);
			}))
		{
			Record.bRevisionComplete = false;
		}
		Record.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(RecordCanonical(Record));
		Record.bRevisionComplete &= IsCanonicalSha256(Record.Revision);
		Snapshot.bComplete &= Record.bRevisionComplete;
	}
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.animation.snapshot.v1"));
	AppendToken(Canonical, Snapshot.ScopeFingerprint);
	AppendBool(Canonical, Snapshot.bComplete);
	AppendInt(Canonical, Snapshot.Records.Num());
	for (const auto& Record : Snapshot.Records)
	{
		AppendToken(Canonical, Record.AssetPath);
		AppendToken(Canonical, Record.Revision);
	}
	Snapshot.Revision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	Snapshot.bComplete &= IsCanonicalSha256(Snapshot.Revision);
	return Snapshot.Revision;
}

TArray<FHyperAIAnimationIssue> FHyperAIStudioAnimationContracts::ValidateValueSnapshot(
	const FHyperAIStudioAnimationValueSnapshot& Snapshot,
	const bool bRequirePackagesClean,
	const int32 MaxIssueCount,
	bool& bOutTruncated)
{
	using namespace HyperAIStudio::Animation::Private;
	TArray<FHyperAIAnimationIssue> Issues;
	bOutTruncated = false;
	const int32 Limit = FMath::Clamp(MaxIssueCount, 1, MaxIssues);
	auto Issue = [&](const TCHAR* Code, const TCHAR* Severity,
		const FHyperAIAnimationAssetRecord& Record, const FString& StableId, const FString& Message)
	{
		AddIssue(Issues, Limit, bOutTruncated, Code, Severity, Record.Variant,
			Record.AssetPath, StableId, -1, Message);
	};
	if (!Snapshot.bComplete)
	{
		FHyperAIAnimationAssetRecord Synthetic;
		Synthetic.Variant = TEXT("snapshot");
		Synthetic.AssetPath = TEXT("/Game/HyperAI/LoadedScope.LoadedScope");
		Issue(TEXT("revision_incomplete"), TEXT("error"), Synthetic, TEXT("snapshot"),
			TEXT("Loaded-state capture is incomplete; validation and CAS fail closed."));
	}
	TSet<FString> RecordIds;
	for (const auto& Record : Snapshot.Records)
	{
		if (RecordIds.Contains(Record.StableId))
			Issue(TEXT("duplicate_record_id"), TEXT("error"), Record, Record.StableId,
				TEXT("Stable asset identity is duplicated."));
		RecordIds.Add(Record.StableId);
		if (!IsCanonicalProjectObjectPath(Record.AssetPath) || !IsCanonicalSha256(Record.Revision)
			|| !Record.bRevisionComplete)
			Issue(TEXT("invalid_asset_revision"), TEXT("error"), Record, Record.StableId,
				TEXT("Asset path or immutable revision evidence is invalid/incomplete."));
		if (bRequirePackagesClean && Record.bPackageDirty)
			Issue(TEXT("package_dirty"), TEXT("error"), Record, Record.StableId,
				TEXT("Validation requires a clean package."));
		if (!FMath::IsFinite(Record.PlayLengthSeconds))
			Issue(TEXT("non_finite_asset_numeric"), TEXT("error"), Record, Record.StableId,
				TEXT("Animation asset contains a non-finite timeline value."));
		if (Record.Variant == TEXT("animation_blueprint"))
		{
			if (!Record.bTemplate && Record.SkeletonPath.IsEmpty())
				Issue(TEXT("missing_target_skeleton"), TEXT("error"), Record, Record.StableId,
					TEXT("Non-template Animation Blueprint has no target Skeleton."));
			if (Record.GeneratedClassPath.IsEmpty())
				Issue(TEXT("generated_class_missing"), TEXT("error"), Record, Record.StableId,
					TEXT("Animation Blueprint has no generated class."));
			if (Record.BlueprintStatus == TEXT("error"))
				Issue(TEXT("blueprint_compile_error"), TEXT("error"), Record, Record.StableId,
					TEXT("Animation Blueprint reports a compile error."));
			else if (Record.BlueprintStatus != TEXT("up_to_date")
				&& Record.BlueprintStatus != TEXT("up_to_date_with_warnings"))
				Issue(TEXT("blueprint_not_compiled"), TEXT("warning"), Record, Record.StableId,
					TEXT("Animation Blueprint is not in a compiled up-to-date state."));
		}
		else if (Record.Variant != TEXT("skeleton") && Record.Variant != TEXT("skeletal_mesh")
			&& Record.SkeletonPath.IsEmpty())
		{
			Issue(TEXT("missing_skeleton"), TEXT("error"), Record, Record.StableId,
				TEXT("Animation asset has no Skeleton reference."));
		}
		if ((Record.Variant == TEXT("skeleton") || Record.Variant == TEXT("skeletal_mesh"))
			&& Record.BoneCount <= 0)
			Issue(TEXT("empty_reference_skeleton"), TEXT("error"), Record, Record.StableId,
				TEXT("Skeleton or Skeletal Mesh has no reference bones."));
		if ((Record.Variant == TEXT("animation_sequence") || Record.Variant == TEXT("montage")
			|| Record.Variant == TEXT("anim_composite")) && Record.PlayLengthSeconds <= 0.0)
			Issue(TEXT("non_positive_play_length"), TEXT("warning"), Record, Record.StableId,
				TEXT("Animation timeline has a non-positive play length."));

		TSet<FString> ElementIds;
		TSet<FString> SectionNames;
		TSet<FString> CurveNames;
		TSet<FString> BoneNames;
		for (const auto& Element : Record.Elements)
		{
			if (!FMath::IsFinite(Element.TimeSeconds) || !FMath::IsFinite(Element.DurationSeconds))
				Issue(TEXT("non_finite_element_numeric"), TEXT("error"), Record, Element.StableId,
					TEXT("Animation element contains a non-finite time, duration, or numeric range value."));
			if (Element.StableId.IsEmpty() || ElementIds.Contains(Element.StableId))
				Issue(TEXT("duplicate_or_empty_element_id"), TEXT("error"), Record, Element.StableId,
					TEXT("Nested stable identity is empty or duplicated."));
			ElementIds.Add(Element.StableId);
			if (Element.Kind == TEXT("transition")
				&& (Element.Name.IsEmpty() || Element.SecondaryName.IsEmpty()))
				Issue(TEXT("broken_transition_endpoint"), TEXT("error"), Record, Element.StableId,
					TEXT("Animation state transition has a missing source or destination."));
			if (Element.Kind == TEXT("montage_section"))
			{
				if (Element.Name.IsEmpty() || SectionNames.Contains(Element.Name))
					Issue(TEXT("duplicate_or_empty_section"), TEXT("error"), Record, Element.StableId,
						TEXT("Montage section name is empty or duplicated."));
				SectionNames.Add(Element.Name);
				if (Element.TimeSeconds < 0.0 || Element.TimeSeconds > Record.PlayLengthSeconds)
					Issue(TEXT("section_time_out_of_range"), TEXT("error"), Record, Element.StableId,
						TEXT("Montage section start is outside the timeline."));
			}
			if (Element.Kind == TEXT("notify"))
			{
				if (Element.TimeSeconds < 0.0 || Element.DurationSeconds < 0.0
					|| Element.TimeSeconds + Element.DurationSeconds > Record.PlayLengthSeconds + KINDA_SMALL_NUMBER)
					Issue(TEXT("notify_time_out_of_range"), TEXT("error"), Record, Element.StableId,
						TEXT("Notify timing is outside the animation timeline."));
			}
			if ((Element.Kind == TEXT("montage_segment")
				|| Element.Kind == TEXT("composite_segment") || Element.Kind == TEXT("blend_sample"))
				&& Element.ReferencePath.IsEmpty())
				Issue(TEXT("missing_animation_reference"), TEXT("error"), Record, Element.StableId,
					TEXT("Segment or sample has no animation reference."));
			if (Element.Kind == TEXT("mesh_skeleton_compatibility") && !Element.bFlag)
				Issue(TEXT("mesh_skeleton_incompatible"), TEXT("error"), Record, Element.StableId,
					TEXT("Skeletal Mesh does not satisfy Unreal's parent-chain Skeleton compatibility check."));
			if (Element.Kind.StartsWith(TEXT("curve.")) || Element.Kind == TEXT("pose_curve"))
			{
				const FString Key = Element.Kind + TEXT(":") + Element.Name;
				if (Element.Name.IsEmpty() || CurveNames.Contains(Key))
					Issue(TEXT("duplicate_or_empty_curve"), TEXT("error"), Record, Element.StableId,
						TEXT("Curve name is empty or duplicated within its type."));
				CurveNames.Add(Key);
			}
			if (Element.Kind == TEXT("skeleton_bone") || Element.Kind == TEXT("mesh_bone"))
			{
				if (Element.Name.IsEmpty() || BoneNames.Contains(Element.Name))
					Issue(TEXT("duplicate_or_empty_bone"), TEXT("error"), Record, Element.StableId,
						TEXT("Reference bone name is empty or duplicated."));
				BoneNames.Add(Element.Name);
				if (Element.Index == 0 && Element.SecondaryIndex != INDEX_NONE)
					Issue(TEXT("invalid_root_parent"), TEXT("error"), Record, Element.StableId,
						TEXT("Root reference bone must have no parent."));
				if (Element.Index > 0 && (Element.SecondaryIndex < 0 || Element.SecondaryIndex >= Element.Index))
					Issue(TEXT("invalid_bone_parent"), TEXT("error"), Record, Element.StableId,
						TEXT("Reference bone parent must precede the child."));
			}
		}
		for (const auto& Element : Record.Elements)
		{
			if (Element.Kind == TEXT("montage_section") && !Element.SecondaryName.IsEmpty()
				&& !SectionNames.Contains(Element.SecondaryName))
				Issue(TEXT("missing_linked_section"), TEXT("error"), Record, Element.StableId,
					TEXT("Montage section links to a missing section."));
		}
	}
	return Issues;
}

FString FHyperAIStudioAnimationContracts::ComputePayloadSemanticFingerprint(
	const TArray<FHyperAIStudioAnimationBackendOperation>& Operations,
	const FString& BaseRevision)
{
	using namespace HyperAIStudio::Animation::Private;
	if (!IsCanonicalSha256(BaseRevision) || Operations.IsEmpty() || Operations.Num() > MaxOperations)
		return FString();
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.animation.payload.v1"));
	AppendToken(Canonical, BaseRevision);
	AppendInt(Canonical, Operations.Num());
	for (const auto& Operation : Operations)
		AppendToken(Canonical, BackendOperationCanonical(Operation));
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

FString FHyperAIStudioAnimationContracts::ComputeStageId(
	const FString& CanonicalProjectId,
	const FString& OperationId,
	const FString& PlanHash,
	const FString& EffectFingerprint)
{
	using namespace HyperAIStudio::Animation::Private;
	if (CanonicalProjectId.IsEmpty()
		|| !FHyperAIStudioExtensionRuntime::IsValidOperationId(OperationId)
		|| !IsCanonicalSha256(PlanHash) || !IsCanonicalSha256(EffectFingerprint)) return FString();
	FString Canonical;
	AppendToken(Canonical, TEXT("hyperai.animation.stage.v1"));
	AppendToken(Canonical, CanonicalProjectId);
	AppendToken(Canonical, OperationId);
	AppendToken(Canonical, PlanHash);
	AppendToken(Canonical, EffectFingerprint);
	return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
}

namespace HyperAIStudio::Animation::Private
{
	FString ExpectedTargetVariant(const EHyperAIStudioAnimationOperationKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioAnimationOperationKind::AnimBlueprintCreate:
		case EHyperAIStudioAnimationOperationKind::AnimBlueprintSetSkeleton:
		case EHyperAIStudioAnimationOperationKind::AnimBlueprintAddStateMachine:
		case EHyperAIStudioAnimationOperationKind::AnimBlueprintAddState:
		case EHyperAIStudioAnimationOperationKind::AnimBlueprintAddTransition:
		case EHyperAIStudioAnimationOperationKind::AnimBlueprintSetTransitionRule:
		case EHyperAIStudioAnimationOperationKind::AnimBlueprintAddInterface:
		case EHyperAIStudioAnimationOperationKind::AnimBlueprintAddLayer:
		case EHyperAIStudioAnimationOperationKind::AnimBlueprintCompileRepair:
			return TEXT("animation_blueprint");
		case EHyperAIStudioAnimationOperationKind::MontageCreate:
		case EHyperAIStudioAnimationOperationKind::MontageAddSection:
		case EHyperAIStudioAnimationOperationKind::MontageRemoveSection:
		case EHyperAIStudioAnimationOperationKind::MontageLinkSections:
		case EHyperAIStudioAnimationOperationKind::MontageAddNotify:
		case EHyperAIStudioAnimationOperationKind::MontageRemoveNotify:
			return TEXT("montage");
		case EHyperAIStudioAnimationOperationKind::CompositeCreate:
		case EHyperAIStudioAnimationOperationKind::CompositeSetSegments:
			return TEXT("anim_composite");
		case EHyperAIStudioAnimationOperationKind::BlendSpaceCreate:
		case EHyperAIStudioAnimationOperationKind::BlendSpaceSetAxis:
		case EHyperAIStudioAnimationOperationKind::BlendSpaceSetSamples:
			return TEXT("blend_space");
		case EHyperAIStudioAnimationOperationKind::PoseAssetCreate:
		case EHyperAIStudioAnimationOperationKind::PoseAssetSetSource:
		case EHyperAIStudioAnimationOperationKind::PoseAssetSetPoseNames:
			return TEXT("pose_asset");
		case EHyperAIStudioAnimationOperationKind::SequenceAddCurve:
		case EHyperAIStudioAnimationOperationKind::SequenceRemoveCurve:
		case EHyperAIStudioAnimationOperationKind::SequenceAddNotify:
		case EHyperAIStudioAnimationOperationKind::SequenceRemoveNotify:
			return TEXT("animation_sequence");
		case EHyperAIStudioAnimationOperationKind::SkeletonAddCompatible:
		case EHyperAIStudioAnimationOperationKind::SkeletonRemoveCompatible:
			return TEXT("skeleton");
		default:
			return FString();
		}
	}

	bool IsCompileOperation(const EHyperAIStudioAnimationOperationKind Kind)
	{
		return Kind == EHyperAIStudioAnimationOperationKind::AnimBlueprintCreate
			|| Kind == EHyperAIStudioAnimationOperationKind::AnimBlueprintSetSkeleton
			|| Kind == EHyperAIStudioAnimationOperationKind::AnimBlueprintAddStateMachine
			|| Kind == EHyperAIStudioAnimationOperationKind::AnimBlueprintAddState
			|| Kind == EHyperAIStudioAnimationOperationKind::AnimBlueprintAddTransition
			|| Kind == EHyperAIStudioAnimationOperationKind::AnimBlueprintSetTransitionRule
			|| Kind == EHyperAIStudioAnimationOperationKind::AnimBlueprintAddInterface
			|| Kind == EHyperAIStudioAnimationOperationKind::AnimBlueprintAddLayer
			|| Kind == EHyperAIStudioAnimationOperationKind::AnimBlueprintCompileRepair;
	}

	bool IsGraphOperation(const EHyperAIStudioAnimationOperationKind Kind)
	{
		return IsCompileOperation(Kind)
			&& Kind != EHyperAIStudioAnimationOperationKind::AnimBlueprintSetSkeleton;
	}

	bool IsTimelineOperation(const EHyperAIStudioAnimationOperationKind Kind)
	{
		return (Kind >= EHyperAIStudioAnimationOperationKind::MontageAddSection
			&& Kind <= EHyperAIStudioAnimationOperationKind::SequenceRemoveNotify)
			&& Kind != EHyperAIStudioAnimationOperationKind::MontageCreate
			&& Kind != EHyperAIStudioAnimationOperationKind::CompositeCreate
			&& Kind != EHyperAIStudioAnimationOperationKind::BlendSpaceCreate
			&& Kind != EHyperAIStudioAnimationOperationKind::PoseAssetCreate;
	}

	bool ValidateLoadedReferences(
		const FHyperAIStudioAnimationBackendOperation& Operation,
		FString& OutCode,
		FString& OutError)
	{
		OutCode.Reset();
		OutError.Reset();
		auto Fail = [&](const TCHAR* Code, const TCHAR* Error)
		{
			OutCode = Code;
			OutError = Error;
			return false;
		};
		auto FindExact = [](const FString& Path) -> UObject*
		{
			if (Path.IsEmpty()) return nullptr;
			UObject* Object = FindObject<UObject>(nullptr, *Path);
			return IsValid(Object) && Object->GetPathName() == Path ? Object : nullptr;
		};
		switch (Operation.Kind)
		{
		case EHyperAIStudioAnimationOperationKind::AnimBlueprintCreate:
		case EHyperAIStudioAnimationOperationKind::AnimBlueprintSetSkeleton:
		case EHyperAIStudioAnimationOperationKind::MontageCreate:
		case EHyperAIStudioAnimationOperationKind::CompositeCreate:
		case EHyperAIStudioAnimationOperationKind::BlendSpaceCreate:
		case EHyperAIStudioAnimationOperationKind::PoseAssetCreate:
			if (!Cast<USkeleton>(FindExact(Operation.ReferencePath)))
				return Fail(TEXT("skeleton_reference_not_loaded"),
					TEXT("Skeleton reference is not already loaded; no disk load was attempted."));
			break;
		case EHyperAIStudioAnimationOperationKind::AnimBlueprintAddInterface:
		{
			const UAnimBlueprint* Interface = Cast<UAnimBlueprint>(FindExact(Operation.ReferencePath));
			if (!Interface || Interface->BlueprintType != BPTYPE_Interface)
				return Fail(TEXT("animation_interface_not_loaded"),
					TEXT("Reference is not an already-loaded Animation Blueprint interface."));
			break;
		}
		case EHyperAIStudioAnimationOperationKind::CompositeSetSegments:
			for (const auto& Segment : Operation.Segments)
			{
				if (!Cast<UAnimSequence>(FindExact(Segment.SequencePath)))
					return Fail(TEXT("segment_sequence_not_loaded"),
						TEXT("Every composite segment sequence must already be loaded."));
			}
			break;
		case EHyperAIStudioAnimationOperationKind::BlendSpaceSetSamples:
			for (const auto& Sample : Operation.Samples)
			{
				if (!Cast<UAnimSequence>(FindExact(Sample.SequencePath)))
					return Fail(TEXT("sample_sequence_not_loaded"),
						TEXT("Every blend-space sample sequence must already be loaded."));
			}
			break;
		case EHyperAIStudioAnimationOperationKind::PoseAssetSetSource:
			if (!Cast<UAnimSequence>(FindExact(Operation.ReferencePath)))
				return Fail(TEXT("pose_source_not_loaded"),
					TEXT("Pose source Animation Sequence must already be loaded."));
			break;
		case EHyperAIStudioAnimationOperationKind::SkeletonAddCompatible:
		case EHyperAIStudioAnimationOperationKind::SkeletonRemoveCompatible:
			if (!Cast<USkeleton>(FindExact(Operation.ReferencePath)))
				return Fail(TEXT("compatible_skeleton_not_loaded"),
					TEXT("Compatible Skeleton reference must already be loaded."));
			break;
		default:
			break;
		}
		return true;
	}

	int32 EstimateRecordBytes(const FHyperAIAnimationAssetRecord& Record)
	{
		// Six bytes per UTF-16 code unit safely covers JSON \uXXXX escaping; fixed charges
		// cover field names, punctuation, booleans, and numeric text.
		int64 Bytes = 512 + 6ll * (Record.Variant.Len() + Record.StableId.Len()
			+ Record.AssetPath.Len() + Record.Revision.Len() + Record.SkeletonPath.Len()
			+ Record.PreviewMeshPath.Len() + Record.ParentClassPath.Len()
			+ Record.GeneratedClassPath.Len() + Record.BlueprintStatus.Len());
		for (const auto& Element : Record.Elements)
			Bytes += 192 + 6ll * (Element.Kind.Len() + Element.StableId.Len() + Element.Name.Len()
			+ Element.SecondaryName.Len() + Element.ContainerName.Len()
			+ Element.ClassPath.Len() + Element.ReferencePath.Len());
		return static_cast<int32>(FMath::Min<int64>(Bytes, MAX_int32));
	}

	int32 EstimateIssueBytes(const FHyperAIAnimationIssue& Issue)
	{
		const int64 Bytes = 160 + 6ll * (Issue.Code.Len() + Issue.Severity.Len()
			+ Issue.Variant.Len() + Issue.AssetPath.Len() + Issue.StableId.Len()
			+ Issue.Message.Len());
		return static_cast<int32>(FMath::Min<int64>(Bytes, MAX_int32));
	}

	int32 EstimateVariantBytes(const FHyperAIAnimationVariantStatus& Status)
	{
		int64 Bytes = 256 + 6ll * (Status.Variant.Len() + Status.State.Len()
			+ Status.Remediation.Len());
		auto AddStrings = [&](const TArray<FString>& Values)
		{
			for (const FString& Value : Values) Bytes += 32 + 6ll * Value.Len();
		};
		AddStrings(Status.RequiredPlugins);
		AddStrings(Status.RequiredModules);
		AddStrings(Status.SupportedCases);
		AddStrings(Status.DelegatedEpicCases);
		AddStrings(Status.UnsupportedCases);
		return static_cast<int32>(FMath::Min<int64>(Bytes, MAX_int32));
	}

	struct FLogicalAnimationState
	{
		FString Variant;
		FString SkeletonPath;
		FString PoseSourcePath;
		double PlayLengthSeconds = 0.0;
		TSet<FString> StateMachines;
		TMap<FString, TSet<FString>> StatesByMachine;
		TSet<FString> Transitions;
		TSet<FString> Layers;
		TSet<FString> Interfaces;
		TSet<FString> Sections;
		TSet<FString> NotifyIds;
		TSet<FString> Curves;
		TSet<FString> PoseNames;
		TSet<FString> CompatibleSkeletons;
		TMap<int32, TPair<double, double>> BlendAxes;
	};

	FString TransitionKey(const FString& Machine, const FString& Source, const FString& Target)
	{
		FString Key;
		AppendToken(Key, Machine);
		AppendToken(Key, Source);
		AppendToken(Key, Target);
		return Key;
	}

	FLogicalAnimationState MakeLogicalState(const FHyperAIAnimationAssetRecord& Record)
	{
		FLogicalAnimationState State;
		State.Variant = Record.Variant;
		State.SkeletonPath = Record.SkeletonPath;
		State.PlayLengthSeconds = Record.PlayLengthSeconds;
		for (const FHyperAIAnimationElementView& Element : Record.Elements)
		{
			if (Element.Kind == TEXT("state_machine")
				|| Element.Kind == TEXT("state_machine_reference")) State.StateMachines.Add(Element.Name);
			else if (Element.Kind == TEXT("state") && !Element.ContainerName.IsEmpty())
				State.StatesByMachine.FindOrAdd(Element.ContainerName).Add(Element.Name);
			else if (Element.Kind == TEXT("transition") && !Element.ContainerName.IsEmpty())
				State.Transitions.Add(TransitionKey(
					Element.ContainerName, Element.Name, Element.SecondaryName));
			else if (Element.Kind == TEXT("animation_layer_graph")) State.Layers.Add(Element.Name);
			else if (Element.Kind == TEXT("animation_interface")) State.Interfaces.Add(Element.ReferencePath);
			else if (Element.Kind == TEXT("montage_section")) State.Sections.Add(Element.Name);
			else if (Element.Kind == TEXT("notify")) State.NotifyIds.Add(Element.StableId);
			else if (Element.Kind.StartsWith(TEXT("curve."))) State.Curves.Add(Element.Name);
			else if (Element.Kind == TEXT("pose")) State.PoseNames.Add(Element.Name);
			else if (Element.Kind == TEXT("pose_source")) State.PoseSourcePath = Element.ReferencePath;
			else if (Element.Kind == TEXT("compatible_skeleton"))
				State.CompatibleSkeletons.Add(Element.ReferencePath);
			else if (Element.Kind == TEXT("blend_axis"))
				State.BlendAxes.Add(Element.Index,
					TPair<double, double>(Element.TimeSeconds, Element.DurationSeconds));
		}
		return State;
	}

	bool ValidateLogicalPlan(
		const TArray<FHyperAIStudioAnimationBackendOperation>& Operations,
		const TMap<FString, FHyperAIAnimationAssetRecord>& RecordByPath,
		TArray<FHyperAIAnimationIssue>& Issues,
		const int32 IssueLimit,
		bool& bTruncated)
	{
		TMap<FString, FLogicalAnimationState> States;
		for (const auto& Pair : RecordByPath) States.Add(Pair.Key, MakeLogicalState(Pair.Value));
		bool bValid = true;
		for (int32 Index = 0; Index < Operations.Num(); ++Index)
		{
			const FHyperAIStudioAnimationBackendOperation& Operation = Operations[Index];
			auto Fail = [&](const TCHAR* Code, const FString& Message)
			{
				bValid = false;
				AddIssue(Issues, IssueLimit, bTruncated, Code, TEXT("error"),
					ExpectedTargetVariant(Operation.Kind), Operation.TargetPath,
					TEXT("animation-operation:") + FString::FromInt(Index), Index, Message);
			};
			if (Operation.bCreatesTarget)
			{
				FLogicalAnimationState Created;
				Created.Variant = ExpectedTargetVariant(Operation.Kind);
				Created.SkeletonPath = Operation.ReferencePath;
				States.Add(Operation.TargetPath, MoveTemp(Created));
				continue;
			}
			FLogicalAnimationState* State = States.Find(Operation.TargetPath);
			if (!State)
			{
				Fail(TEXT("logical_target_state_missing"),
					TEXT("No captured or same-plan-created logical target state exists."));
				continue;
			}
			switch (Operation.Kind)
			{
			case EHyperAIStudioAnimationOperationKind::AnimBlueprintSetSkeleton:
				State->SkeletonPath = Operation.ReferencePath;
				break;
			case EHyperAIStudioAnimationOperationKind::AnimBlueprintAddStateMachine:
				if (State->StateMachines.Contains(Operation.Name))
					Fail(TEXT("state_machine_already_exists"), TEXT("Animation Blueprint already contains that state machine."));
				else State->StateMachines.Add(Operation.Name);
				break;
			case EHyperAIStudioAnimationOperationKind::AnimBlueprintAddState:
				if (!State->StateMachines.Contains(Operation.Name))
					Fail(TEXT("state_machine_missing"), TEXT("State creation requires an existing or earlier same-plan state machine."));
				else if (State->StatesByMachine.FindOrAdd(Operation.Name).Contains(Operation.SecondaryName))
					Fail(TEXT("state_already_exists"), TEXT("State machine already contains that state."));
				else State->StatesByMachine.FindOrAdd(Operation.Name).Add(Operation.SecondaryName);
				break;
			case EHyperAIStudioAnimationOperationKind::AnimBlueprintAddTransition:
			{
				const FString& Source = Operation.Mappings[0].Source;
				const FString& Target = Operation.Mappings[0].Target;
				const TSet<FString>* MachineStates = State->StatesByMachine.Find(Operation.Name);
				const FString Key = TransitionKey(Operation.Name, Source, Target);
				if (!State->StateMachines.Contains(Operation.Name) || !MachineStates
					|| !MachineStates->Contains(Source) || !MachineStates->Contains(Target))
					Fail(TEXT("transition_endpoint_missing"), TEXT("Transition requires one existing machine and two existing endpoint states."));
				else if (State->Transitions.Contains(Key))
					Fail(TEXT("transition_already_exists"), TEXT("The exact state transition already exists."));
				else
				{
					State->Transitions.Add(Key);
					if (Operation.bFlag) State->Transitions.Add(TransitionKey(Operation.Name, Target, Source));
				}
				break;
			}
			case EHyperAIStudioAnimationOperationKind::AnimBlueprintSetTransitionRule:
				if (!State->Transitions.Contains(TransitionKey(Operation.Name,
					Operation.Mappings[0].Source, Operation.Mappings[0].Target)))
					Fail(TEXT("transition_missing"), TEXT("Transition-rule edit requires an existing or earlier same-plan transition."));
				break;
			case EHyperAIStudioAnimationOperationKind::AnimBlueprintAddInterface:
				if (State->Interfaces.Contains(Operation.ReferencePath))
					Fail(TEXT("animation_interface_already_exists"), TEXT("Animation Blueprint already implements that interface."));
				else State->Interfaces.Add(Operation.ReferencePath);
				break;
			case EHyperAIStudioAnimationOperationKind::AnimBlueprintAddLayer:
				if (State->Layers.Contains(Operation.Name))
					Fail(TEXT("animation_layer_already_exists"), TEXT("Animation Blueprint already contains that layer graph."));
				else State->Layers.Add(Operation.Name);
				break;
			case EHyperAIStudioAnimationOperationKind::MontageAddSection:
				if (State->Sections.Contains(Operation.Name))
					Fail(TEXT("montage_section_already_exists"), TEXT("Montage section name already exists."));
				else if (State->PlayLengthSeconds > 0.0 && Operation.Minimum > State->PlayLengthSeconds)
					Fail(TEXT("montage_section_time_out_of_range"), TEXT("Montage section start exceeds the captured timeline."));
				else State->Sections.Add(Operation.Name);
				break;
			case EHyperAIStudioAnimationOperationKind::MontageRemoveSection:
				if (!State->Sections.Remove(Operation.Name))
					Fail(TEXT("montage_section_missing"), TEXT("Montage section removal requires an existing section."));
				break;
			case EHyperAIStudioAnimationOperationKind::MontageLinkSections:
				if (!State->Sections.Contains(Operation.Name)
					|| (!Operation.SecondaryName.IsEmpty() && !State->Sections.Contains(Operation.SecondaryName)))
					Fail(TEXT("montage_section_link_target_missing"), TEXT("Section linking requires existing source and optional destination sections."));
				break;
			case EHyperAIStudioAnimationOperationKind::MontageAddNotify:
			case EHyperAIStudioAnimationOperationKind::SequenceAddNotify:
				if (State->PlayLengthSeconds > 0.0
					&& Operation.Notify.TimeSeconds + Operation.Notify.DurationSeconds
						> State->PlayLengthSeconds + KINDA_SMALL_NUMBER)
					Fail(TEXT("notify_time_out_of_range"), TEXT("Notify timing exceeds the captured animation timeline."));
				break;
			case EHyperAIStudioAnimationOperationKind::MontageRemoveNotify:
			case EHyperAIStudioAnimationOperationKind::SequenceRemoveNotify:
				if (!State->NotifyIds.Remove(Operation.StableId))
					Fail(TEXT("notify_selector_missing"), TEXT("Stable notify selector is absent from the exact captured revision."));
				break;
			case EHyperAIStudioAnimationOperationKind::BlendSpaceSetAxis:
				State->BlendAxes.Add(Operation.AxisIndex,
					TPair<double, double>(Operation.Minimum, Operation.Maximum));
				break;
			case EHyperAIStudioAnimationOperationKind::BlendSpaceSetSamples:
				for (const FHyperAIAnimationBlendSampleSpec& Sample : Operation.Samples)
				{
					const double Coordinates[3] = {Sample.Position.X, Sample.Position.Y, Sample.Position.Z};
					for (int32 Axis = 0; Axis < 3; ++Axis)
					{
						const TPair<double, double>* Range = State->BlendAxes.Find(Axis);
						if (!Range || Coordinates[Axis] < Range->Key || Coordinates[Axis] > Range->Value)
						{
							Fail(TEXT("blend_sample_outside_axis"), TEXT("Every blend sample coordinate must lie inside the current or earlier same-plan axis range."));
							break;
						}
					}
				}
				break;
			case EHyperAIStudioAnimationOperationKind::PoseAssetSetSource:
				State->PoseSourcePath = Operation.ReferencePath;
				break;
			case EHyperAIStudioAnimationOperationKind::PoseAssetSetPoseNames:
				for (const FHyperAIAnimationNameMapping& Mapping : Operation.Mappings)
				{
					if (!State->PoseNames.Contains(Mapping.Source))
						Fail(TEXT("pose_name_source_missing"), TEXT("Pose rename source is absent from the exact captured revision."));
					else if (Mapping.Source != Mapping.Target && State->PoseNames.Contains(Mapping.Target))
						Fail(TEXT("pose_name_target_exists"), TEXT("Pose rename target already exists."));
					else
					{
						State->PoseNames.Remove(Mapping.Source);
						State->PoseNames.Add(Mapping.Target);
					}
				}
				break;
			case EHyperAIStudioAnimationOperationKind::SequenceAddCurve:
				if (State->Curves.Contains(Operation.Name))
					Fail(TEXT("curve_already_exists"), TEXT("Animation Sequence already contains that curve name."));
				else State->Curves.Add(Operation.Name);
				break;
			case EHyperAIStudioAnimationOperationKind::SequenceRemoveCurve:
				if (!State->Curves.Remove(Operation.Name))
					Fail(TEXT("curve_missing"), TEXT("Curve removal requires an existing curve name."));
				break;
			case EHyperAIStudioAnimationOperationKind::SkeletonAddCompatible:
				if (State->CompatibleSkeletons.Contains(Operation.ReferencePath))
					Fail(TEXT("compatible_skeleton_already_present"), TEXT("Skeleton compatibility edge already exists."));
				else State->CompatibleSkeletons.Add(Operation.ReferencePath);
				break;
			case EHyperAIStudioAnimationOperationKind::SkeletonRemoveCompatible:
				if (!State->CompatibleSkeletons.Remove(Operation.ReferencePath))
					Fail(TEXT("compatible_skeleton_missing"), TEXT("Compatibility removal requires an existing edge."));
				break;
			default:
				break;
			}
		}
		return bValid;
	}

	bool ParseCursor(const FString& Cursor, FString& OutRevision, int32& OutOffset)
	{
		OutRevision.Reset();
		OutOffset = 0;
		if (Cursor.IsEmpty()) return true;
		if (!Cursor.StartsWith(TEXT("revision:"), ESearchCase::CaseSensitive)) return false;
		FString RevisionPart;
		FString Number;
		if (!Cursor.Mid(9).Split(TEXT("|offset:"), &RevisionPart, &Number,
			ESearchCase::CaseSensitive, ESearchDir::FromEnd)
			|| !FHyperAIStudioAnimationContracts::IsCanonicalSha256(RevisionPart)) return false;
		if (Number.IsEmpty() || Number.Len() > 10
			|| !Number.IsNumeric()) return false;
		OutRevision = MoveTemp(RevisionPart);
		OutOffset = FCString::Atoi(*Number);
		return OutOffset >= 0;
	}
}

FHyperAIAnimationApplyPlanReport FHyperAIStudioAnimationContracts::BuildPlan(
	const FHyperAIAnimationApplyPlanRequest& Request)
{
	using namespace HyperAIStudio::Animation::Private;
	FHyperAIAnimationApplyPlanReport Report;
	Report.bDryRun = Request.bDryRun;
	Report.OperationId = Request.OperationId;
	const double PlanDeadlineSeconds = FPlatformTime::Seconds()
		+ static_cast<double>(FMath::Clamp(Request.MaxGameThreadMs, 1, 250)) / 1000.0;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("game_thread_required");
		Report.Diagnostic = TEXT("Animation UObject planning is allowed only on Unreal's serialized game thread.");
		return Report;
	}
	if (Request.Operations.IsEmpty() || Request.Operations.Num() > MaxOperations
		|| Request.DeadlineMs < 100 || Request.DeadlineMs > 2000
		|| Request.MaxGameThreadMs < 50 || Request.MaxGameThreadMs > 250
		|| Request.MaxOutputBytes < MinOutputBytes || Request.MaxOutputBytes > MaxOutputBytes
		|| Request.OperationId.Len() > FHyperAIStudioDomainLimits::MaxOperationIdChars
		|| Request.ExpectedPlanHash.Len() > 71)
	{
		Report.Status = TEXT("invalid_request_bounds");
		Report.Diagnostic = TEXT("Animation plan violates operation, deadline, game-thread, or output bounds.");
		return Report;
	}
	if (Request.bDryRun && (!Request.OperationId.IsEmpty() || !Request.ExpectedPlanHash.IsEmpty()))
	{
		Report.Status = TEXT("invalid_dry_run_shape");
		Report.Diagnostic = TEXT("Dry-run requests must leave operation_id and expected_plan_hash empty.");
		return Report;
	}
	int32 EstimatedOutputBytes = 1024;
	const int32 VariantBudget = FMath::Max(0, FMath::Min(Request.MaxOutputBytes / 3,
		Request.MaxOutputBytes - 2048));
	for (const auto& Status : FHyperAIStudioAnimationFacade::ResolveVariantStatuses())
	{
		const int32 Bytes = EstimateVariantBytes(Status);
		if (static_cast<int64>(EstimatedOutputBytes) + Bytes > 1024 + VariantBudget)
		{
			Report.bTruncated = true;
			break;
		}
		EstimatedOutputBytes += Bytes;
		Report.Variants.Add(Status);
	}

	const int32 IssueLimit = FMath::Clamp(
		(Request.MaxOutputBytes - EstimatedOutputBytes - 512) / 3072, 1, MaxIssues);
	TArray<FHyperAIStudioAnimationBackendOperation> Operations;
	TSet<FString> ExactOperations;
	TArray<FString> ExistingPaths;
	TArray<FString> AllTargetPaths;
	TSet<FString> ExistingSet;
	TSet<FString> AllTargetSet;
	TSet<FString> CreateSet;
	TMap<FString, FString> LifecycleVariantByTarget;
	bool bUnavailable = false;
	for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
	{
		if (FPlatformTime::Seconds() >= PlanDeadlineSeconds)
		{
			AddIssue(Report.Issues, IssueLimit, Report.bTruncated,
				TEXT("planning_budget_exceeded"), TEXT("error"), TEXT("apply_plan"),
				FString(), TEXT("animation-plan"), Index,
				TEXT("Closed plan validation exceeded the caller's game-thread budget."));
			break;
		}
		FHyperAIStudioAnimationBackendOperation Backend;
		FString ErrorCode;
		FString Error;
		if (!ValidateOperationShape(Request.Operations[Index], Backend, ErrorCode, Error))
		{
			bUnavailable |= ErrorCode == TEXT("variant_adapter_unavailable");
			AddIssue(Report.Issues, IssueLimit, Report.bTruncated, *ErrorCode, TEXT("error"),
				TEXT("apply_plan"), Request.Operations[Index].TargetPath,
				TEXT("animation-operation:") + FString::FromInt(Index), Index, Error);
			continue;
		}
		const FString Canonical = BackendOperationCanonical(Backend);
		if (ExactOperations.Contains(Canonical))
		{
			AddIssue(Report.Issues, IssueLimit, Report.bTruncated, TEXT("duplicate_operation"),
				TEXT("error"), TEXT("apply_plan"), Backend.TargetPath,
				TEXT("animation-operation:") + FString::FromInt(Index), Index,
				TEXT("Exact duplicate operations are rejected."));
			continue;
		}
		ExactOperations.Add(Canonical);
		if (Backend.bCreatesTarget)
		{
			if (CreateSet.Contains(Backend.TargetPath))
			{
				AddIssue(Report.Issues, IssueLimit, Report.bTruncated, TEXT("target_lifecycle_conflict"),
					TEXT("error"), TEXT("apply_plan"), Backend.TargetPath,
					TEXT("animation-operation:") + FString::FromInt(Index), Index,
					TEXT("One plan cannot create the same target twice."));
				continue;
			}
			if (ExistingSet.Contains(Backend.TargetPath))
			{
				AddIssue(Report.Issues, IssueLimit, Report.bTruncated, TEXT("create_must_precede_edits"),
					TEXT("error"), TEXT("apply_plan"), Backend.TargetPath,
					TEXT("animation-operation:") + FString::FromInt(Index), Index,
					TEXT("A same-plan create must precede every configuration operation for its target."));
				continue;
			}
			UObject* Existing = FindObject<UObject>(nullptr, *Backend.TargetPath);
			if (IsValid(Existing) && Existing->GetPathName() == Backend.TargetPath)
			{
				AddIssue(Report.Issues, IssueLimit, Report.bTruncated, TEXT("create_target_already_loaded"),
					TEXT("error"), TEXT("apply_plan"), Backend.TargetPath,
					TEXT("animation-operation:") + FString::FromInt(Index), Index,
					TEXT("Create target already exists in loaded state."));
				continue;
			}
			FName TargetPackageName;
			IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
			FAssetPackageData ExistingPackageData;
			const UE::AssetRegistry::EExists DiskState =
				!TryGetPrimaryAssetPackageName(Backend.TargetPath, TargetPackageName)
				|| !AssetRegistry || AssetRegistry->IsGathering()
				|| AssetRegistry->IsLoadingAssets()
				? UE::AssetRegistry::EExists::Unknown
				: AssetRegistry->TryGetAssetPackageData(
					TargetPackageName, ExistingPackageData, true /* bFailIfLockHeld */);
			if (DiskState != UE::AssetRegistry::EExists::DoesNotExist)
			{
				AddIssue(Report.Issues, IssueLimit, Report.bTruncated,
					DiskState == UE::AssetRegistry::EExists::Exists
						? TEXT("create_target_exists_on_disk") : TEXT("asset_registry_state_unknown"),
					TEXT("error"), TEXT("apply_plan"), Backend.TargetPath,
					TEXT("animation-operation:") + FString::FromInt(Index), Index,
					DiskState == UE::AssetRegistry::EExists::Exists
						? TEXT("Create target package already exists in the nonblocking no-load Asset Registry view.")
						: TEXT("Asset Registry cannot prove target-package absence without waiting; create precondition fails closed."));
				continue;
			}
			CreateSet.Add(Backend.TargetPath);
			LifecycleVariantByTarget.Add(Backend.TargetPath, ExpectedTargetVariant(Backend.Kind));
		}
		else
		{
			if (CreateSet.Contains(Backend.TargetPath))
			{
				const FString ExpectedVariant = ExpectedTargetVariant(Backend.Kind);
				const FString* CreatedVariant = LifecycleVariantByTarget.Find(Backend.TargetPath);
				if (!Backend.ExpectedRevision.IsEmpty() || !CreatedVariant
					|| *CreatedVariant != ExpectedVariant)
				{
					AddIssue(Report.Issues, IssueLimit, Report.bTruncated,
						TEXT("same_plan_create_binding_invalid"), TEXT("error"), ExpectedVariant,
						Backend.TargetPath, TEXT("animation-operation:") + FString::FromInt(Index),
						Index, TEXT("Configuration after a same-plan create requires an empty revision and the exact created asset variant."));
					continue;
				}
			}
			else
			{
				if (!IsCanonicalSha256(Backend.ExpectedRevision))
				{
					AddIssue(Report.Issues, IssueLimit, Report.bTruncated,
						TEXT("existing_target_revision_required"), TEXT("error"),
						ExpectedTargetVariant(Backend.Kind), Backend.TargetPath,
						TEXT("animation-operation:") + FString::FromInt(Index), Index,
						TEXT("An edit of an existing target requires one canonical loaded-state CAS revision."));
					continue;
				}
				const FString ExpectedVariant = ExpectedTargetVariant(Backend.Kind);
				if (const FString* PriorVariant = LifecycleVariantByTarget.Find(Backend.TargetPath))
				{
					if (*PriorVariant != ExpectedVariant)
					{
						AddIssue(Report.Issues, IssueLimit, Report.bTruncated,
							TEXT("target_variant_lifecycle_conflict"), TEXT("error"),
							ExpectedVariant, Backend.TargetPath,
							TEXT("animation-operation:") + FString::FromInt(Index), Index,
							TEXT("All operations for one target must use the same exact animation asset variant."));
						continue;
					}
				}
				else LifecycleVariantByTarget.Add(Backend.TargetPath, ExpectedVariant);
				if (!ExistingSet.Contains(Backend.TargetPath))
				{
					ExistingSet.Add(Backend.TargetPath);
					ExistingPaths.Add(Backend.TargetPath);
				}
			}
		}
		if (!AllTargetSet.Contains(Backend.TargetPath))
		{
			AllTargetSet.Add(Backend.TargetPath);
			AllTargetPaths.Add(Backend.TargetPath);
		}
		FString ReferenceCode;
		FString ReferenceError;
		if (!ValidateLoadedReferences(Backend, ReferenceCode, ReferenceError))
		{
			AddIssue(Report.Issues, IssueLimit, Report.bTruncated, *ReferenceCode,
				TEXT("error"), ExpectedTargetVariant(Backend.Kind), Backend.TargetPath,
				TEXT("animation-operation:") + FString::FromInt(Index), Index, ReferenceError);
			continue;
		}
		if (IsGraphOperation(Backend.Kind)) ++Report.Effects.GraphChanges;
		if (IsTimelineOperation(Backend.Kind)) ++Report.Effects.TimelineChanges;
		if (Backend.Kind == EHyperAIStudioAnimationOperationKind::SkeletonAddCompatible
			|| Backend.Kind == EHyperAIStudioAnimationOperationKind::SkeletonRemoveCompatible)
			++Report.Effects.CompatibilityChanges;
		Operations.Add(MoveTemp(Backend));
	}
	if (HasErrors(Report.Issues) || Operations.Num() != Request.Operations.Num())
	{
		Report.Status = bUnavailable ? TEXT("variant_adapter_unavailable") : TEXT("invalid_plan_shape");
		Report.Diagnostic = TEXT("One or more closed Animation/Rigging operation variants were invalid or unavailable.");
		return Report;
	}

	FHyperAIStudioAnimationValueSnapshot BaseSnapshot;
	if (!ExistingPaths.IsEmpty())
	{
		FString CaptureStatus;
		FString CaptureDiagnostic;
		const int32 RemainingWorkMs = FMath::Max(0, FMath::FloorToInt(
			(PlanDeadlineSeconds - FPlatformTime::Seconds()) * 1000.0));
		if (RemainingWorkMs < 1
			|| !FHyperAIStudioAnimationFacade::CaptureLoaded(ExistingPaths, TEXT("all"), true,
				BaseSnapshot, CaptureStatus, CaptureDiagnostic, RemainingWorkMs)
			|| !BaseSnapshot.bComplete)
		{
			Report.Status = TEXT("target_state_incomplete");
			Report.Diagnostic = TEXT("Every existing target must be already loaded with a complete base-variant revision.");
			return Report;
		}
	}
	else
	{
		FString EmptyScope;
		AppendToken(EmptyScope, TEXT("hyperai.animation.create-only-scope.v1"));
		BaseSnapshot.ScopeFingerprint = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(EmptyScope);
		ComputeSnapshotRevision(BaseSnapshot);
	}
	TMap<FString, FHyperAIAnimationAssetRecord> RecordByPath;
	for (const auto& Record : BaseSnapshot.Records) RecordByPath.Add(Record.AssetPath, Record);
	if (RecordByPath.Num() != ExistingPaths.Num())
	{
		Report.Status = TEXT("target_state_incomplete");
		Report.Diagnostic = TEXT("One or more existing target paths did not resolve to a supported base animation asset.");
		return Report;
	}
	TMap<FString, FString> RevisionByTarget;
	for (int32 Index = 0; Index < Operations.Num(); ++Index)
	{
		const auto& Operation = Operations[Index];
		if (Operation.bCreatesTarget || CreateSet.Contains(Operation.TargetPath)) continue;
		const FHyperAIAnimationAssetRecord* Record = RecordByPath.Find(Operation.TargetPath);
		const FString ExpectedVariant = ExpectedTargetVariant(Operation.Kind);
		if (!Record || Record->Revision != Operation.ExpectedRevision)
		{
			AddIssue(Report.Issues, IssueLimit, Report.bTruncated, TEXT("revision_precondition_failed"),
				TEXT("error"), ExpectedVariant, Operation.TargetPath,
				TEXT("animation-operation:") + FString::FromInt(Index), Index,
				TEXT("Target is missing or its fresh loaded revision does not match expected_revision."));
			continue;
		}
		if (Record->Variant != ExpectedVariant)
		{
			AddIssue(Report.Issues, IssueLimit, Report.bTruncated, TEXT("target_variant_mismatch"),
				TEXT("error"), Record->Variant, Operation.TargetPath, Record->StableId, Index,
				TEXT("Operation target is not the exact required animation asset variant."));
			continue;
		}
		if (const FString* PriorRevision = RevisionByTarget.Find(Operation.TargetPath))
		{
			if (*PriorRevision != Operation.ExpectedRevision)
				AddIssue(Report.Issues, IssueLimit, Report.bTruncated, TEXT("conflicting_target_revisions"),
					TEXT("error"), Record->Variant, Operation.TargetPath, Record->StableId, Index,
					TEXT("All operations for one target must bind the same exact CAS revision."));
		}
		else RevisionByTarget.Add(Operation.TargetPath, Operation.ExpectedRevision);
	}
	if (!HasErrors(Report.Issues))
	{
		ValidateLogicalPlan(Operations, RecordByPath, Report.Issues, IssueLimit, Report.bTruncated);
	}
	if (FPlatformTime::Seconds() >= PlanDeadlineSeconds)
	{
		Report.Status = TEXT("planning_budget_exceeded");
		Report.Diagnostic = TEXT("Animation plan validation exceeded the caller's game-thread budget before sealing.");
		return Report;
	}
	if (HasErrors(Report.Issues))
	{
		Report.Status = TEXT("plan_state_invalid");
		Report.Diagnostic = TEXT("Target type, loaded-reference, lifecycle, or CAS preconditions failed.");
		return Report;
	}

	AllTargetPaths.Sort();
	FString BaseCanonical;
	AppendToken(BaseCanonical, TEXT("hyperai.animation.base.v1"));
	for (const FString& Path : AllTargetPaths)
	{
		AppendToken(BaseCanonical, Path);
		if (CreateSet.Contains(Path)) AppendToken(BaseCanonical, TEXT("absent"));
		else AppendToken(BaseCanonical, RecordByPath.FindChecked(Path).Revision);
	}
	Report.BaseRevision = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(BaseCanonical);
	const FString SemanticFingerprint = ComputePayloadSemanticFingerprint(Operations, Report.BaseRevision);
	const FString ProjectId = FHyperAIStudioExtensionRuntime::GetCanonicalProjectId();
	if (!IsCanonicalSha256(Report.BaseRevision) || !IsCanonicalSha256(SemanticFingerprint)
		|| ProjectId.IsEmpty())
	{
		Report.Status = TEXT("semantic_identity_unavailable");
		Report.Diagnostic = TEXT("Bounded semantic hashes or canonical project identity could not be produced.");
		return Report;
	}

	const auto Payload = MakeShared<FHyperAIStudioAnimationTypedPayload, ESPMode::ThreadSafe>();
	Payload->Operations = Operations;
	Payload->BaseRevision = Report.BaseRevision;
	Payload->SemanticFingerprint = SemanticFingerprint;
	const int32 PayloadBytes = Payload->GetBoundedByteSize();
	if (PayloadBytes <= 0 || PayloadBytes > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		Report.Status = TEXT("typed_payload_exceeds_shared_bound");
		Report.Diagnostic = TEXT("Closed Animation payload exceeds the shared typed-artifact request bound and cannot be staged unchanged.");
		return Report;
	}
	const FHyperAIStudioDomainAdapterDescriptor& Adapter = GetBaseAdapterDescriptor();
	FHyperAIStudioDomainBinding Binding;
	Binding.PackId = PackId;
	Binding.ToolName = TEXT("hyper_animation_apply_plan");
	Binding.VariantId = MutationVariantId;
	Binding.ExpectedSafety = EHyperAIStudioDomainSafety::Edit;
	Binding.CanonicalProjectId = ProjectId;
	Binding.ExpectedAdapterFingerprint = Adapter.AdapterFingerprint;
	Binding.ExpectedAdapterGeneration = 1;
	Binding.ExpectedRegistryEpoch = 1;
	Binding.Prerequisites.PackId = PackId;
	Binding.Prerequisites.bPackEnabled = true;
	Binding.Prerequisites.Revision = 1;
	Binding.Prerequisites.Observations = {
		{TEXT("module.Engine"), EHyperAIStudioDomainPrerequisiteState::Available},
		{TEXT("module.AnimGraph"), EHyperAIStudioDomainPrerequisiteState::Available},
		{TEXT("module.BlueprintGraph"), EHyperAIStudioDomainPrerequisiteState::Available},
		{TEXT("module.Kismet"), EHyperAIStudioDomainPrerequisiteState::Available},
		{TEXT("module.UnrealEd"), EHyperAIStudioDomainPrerequisiteState::Available}};
	Binding.Prerequisites.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputePrerequisiteFingerprint(Binding.Prerequisites);
	Binding.Admission.PackId = PackId;
	Binding.Admission.bPackAdmitted = false;
	Binding.Admission.bEditAdmitted = false;
	Binding.Admission.Revision = 1;
	Binding.Admission.Fingerprint =
		FHyperAIStudioDomainAdapterRegistry::ComputeAdmissionFingerprint(Binding.Admission);

	FHyperAIStudioTypedArtifactContract Contract;
	Contract.Binding = Binding;
	Contract.ArtifactTypeId = PayloadTypeId;
	Contract.ArtifactSchemaFingerprint = PayloadSchemaFingerprint();
	Contract.ArtifactSemanticFingerprint = SemanticFingerprint;
	Contract.EffectTarget = TEXT("animation:") + Report.BaseRevision;
	Contract.DeadlineMs = Request.DeadlineMs;
	Contract.MaxNativeOperations = FMath::Clamp(Operations.Num() + 4, 5, 128);
	Contract.MaxGameThreadMs = Request.MaxGameThreadMs;
	Contract.MaxOutputBytes = Request.MaxOutputBytes;
	Contract.MaxResultBytes = 256;
	Contract.StageLifetimeMs = StageLifetimeMs;
	Contract.bCompileOnce = Operations.ContainsByPredicate([](const auto& Operation)
	{
		return IsCompileOperation(Operation.Kind);
	});
	Contract.bSaveOnce = true;
	Contract.bValidateOnce = true;
	Contract.bVerifyFreshOnce = true;
	FHyperAIStudioPreparedTypedArtifact Prepared;
	FString PrepareError;
	if (!FHyperAIStudioTypedArtifactExecutor::Prepare(Contract, Prepared, PrepareError))
	{
		Report.Status = TEXT("typed_artifact_prepare_failed");
		Report.Diagnostic = Clip(PrepareError);
		return Report;
	}
	Report.PlanHash = Prepared.PlanHash;
	Report.AuthorizationPlanHash = Prepared.AuthorizationPlanHash;
	Report.CapabilityHash = Prepared.CapabilityHash;
	Report.EffectFingerprint = Prepared.EffectFingerprint;
	Report.Effects.OperationCount = Operations.Num();
	Report.Effects.TargetCount = AllTargetPaths.Num();
	Report.Effects.AssetsCreated = CreateSet.Num();
	Report.Effects.AssetsUpdated = ExistingSet.Num();
	Report.Effects.bTransactionOnce = true;
	Report.Effects.bCompileOnce = Contract.bCompileOnce;
	Report.Effects.bSaveOnce = true;
	Report.Effects.bValidateOnce = true;
	Report.Effects.bFreshVerifyOnce = true;

	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("valid_dry_run");
		Report.Diagnostic = TEXT("Closed Animation/Rigging plan, loaded-state CAS, references, and typed-artifact hashes are valid without mutation.");
		return Report;
	}
	if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId))
	{
		Report.Status = TEXT("invalid_operation_id");
		Report.Diagnostic = TEXT("Staging requires one journal-safe operation_id.");
		return Report;
	}
	if (Request.ExpectedPlanHash != Prepared.PlanHash || !IsCanonicalSha256(Request.ExpectedPlanHash))
	{
		Report.Status = TEXT("expected_plan_hash_mismatch");
		Report.Diagnostic = TEXT("Staging must echo the exact dry-run plan hash.");
		return Report;
	}
	FHyperAIStudioAnimationStagedArtifact Artifact;
	Artifact.Prepared = Prepared;
	Artifact.Payload = Payload;
	Artifact.CanonicalProjectId = ProjectId;
	Artifact.OperationId = Request.OperationId;
	Artifact.StageId = ComputeStageId(ProjectId, Request.OperationId,
		Prepared.PlanHash, Prepared.EffectFingerprint);
	Artifact.ExpiresMonotonicMs = NowMonotonicMs() + StageLifetimeMs;
	FString StageError;
	if (!FHyperAIStudioAnimationStagingService::Stage(Artifact, Report.bReplay, StageError))
	{
		Report.Status = TEXT("stage_rejected");
		Report.Diagnostic = Clip(StageError);
		return Report;
	}
	Report.bOk = false;
	Report.bStaged = true;
	Report.bExecutionSubmitted = false;
	Report.bFallbackPermitted = false;
	Report.StageId = Artifact.StageId;
	Report.Status = TEXT("staged_backend_required");
	Report.Diagnostic = TEXT("Plan is idempotently staged and side-effect-free; no mutation is submitted until the shared async execution host owns its journal and live adapter lease.");
	return Report;
}

FHyperAIAnimationInspectReport UHyperAIStudioAnimationToolset::hyper_animation_inspect(
	const FHyperAIAnimationInspectRequest& Request)
{
	using namespace HyperAIStudio::Animation::Private;
	FHyperAIAnimationInspectReport Report;
	if (Request.AssetPaths.Num() > FHyperAIStudioAnimationContracts::MaxAssetPaths
		|| Request.PageSize < 1 || Request.PageSize > FHyperAIStudioAnimationContracts::MaxPageSize
		|| Request.Cursor.Len() > FHyperAIStudioAnimationContracts::MaxCursorCharacters
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > 250
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > FHyperAIStudioAnimationContracts::MaxOutputBytes)
	{
		Report.Status = TEXT("invalid_request_bounds");
		Report.Diagnostic = TEXT("Animation inspect request exceeds path, page, cursor, or output bounds.");
		return Report;
	}
	int32 Offset = 0;
	FString CursorRevision;
	if (!ParseCursor(Request.Cursor, CursorRevision, Offset))
	{
		Report.Status = TEXT("invalid_cursor");
		Report.Diagnostic = TEXT("Cursor must be empty or bind one exact snapshot revision and offset.");
		return Report;
	}
	FHyperAIStudioAnimationValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!FHyperAIStudioAnimationFacade::CaptureLoaded(Request.AssetPaths, Request.Variant, true,
		Snapshot, CaptureStatus, CaptureDiagnostic, Request.MaxGameThreadMs))
	{
		Report.Status = CaptureStatus;
		Report.Diagnostic = Clip(CaptureDiagnostic);
		if (Request.bIncludePrerequisites)
		{
			int32 FailureBytes = 512;
			for (const auto& Status : FHyperAIStudioAnimationFacade::ResolveVariantStatuses())
			{
				const int32 Bytes = EstimateVariantBytes(Status);
				if (static_cast<int64>(FailureBytes) + Bytes > Request.MaxOutputBytes)
				{
					Report.bTruncated = true;
					break;
				}
				FailureBytes += Bytes;
				Report.Variants.Add(Status);
			}
		}
		return Report;
	}
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	Report.LoadedObjectsScanned = Snapshot.LoadedObjectsScanned;
	Report.TotalRecords = Snapshot.Records.Num();
	if (!CursorRevision.IsEmpty() && CursorRevision != Snapshot.Revision)
	{
		Report.Status = TEXT("stale_cursor_revision");
		Report.Diagnostic = TEXT("Loaded state changed between pages; restart pagination from an empty cursor.");
		return Report;
	}
	if (Offset > Snapshot.Records.Num())
	{
		Report.Status = TEXT("cursor_out_of_range");
		Report.Diagnostic = TEXT("Cursor offset is beyond the current deterministic result set.");
		return Report;
	}
	int32 EstimatedBytes = 2048;
	for (const auto& Issue : Snapshot.CaptureIssues)
	{
		const int32 IssueBytes = EstimateIssueBytes(Issue);
		if (static_cast<int64>(EstimatedBytes) + IssueBytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			break;
		}
		EstimatedBytes += IssueBytes;
		Report.Issues.Add(Issue);
	}
	const int32 End = FMath::Min(Offset + Request.PageSize, Snapshot.Records.Num());
	bool bRecordExceededBudget = false;
	for (int32 Index = Offset; Index < End; ++Index)
	{
		FHyperAIAnimationAssetRecord Record = Snapshot.Records[Index];
		if (!Request.bIncludeDetails)
		{
			Record.bOutputDetailsTruncated = !Record.Elements.IsEmpty();
			Record.Elements.Reset();
		}
		int32 RecordBytes = EstimateRecordBytes(Record);
		if (static_cast<int64>(EstimatedBytes) + RecordBytes > Request.MaxOutputBytes
			&& !Record.Elements.IsEmpty())
		{
			Record.bOutputDetailsTruncated = true;
			Record.Elements.Reset();
			RecordBytes = EstimateRecordBytes(Record);
		}
		if (static_cast<int64>(EstimatedBytes) + RecordBytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			bRecordExceededBudget = true;
			break;
		}
		EstimatedBytes += RecordBytes;
		Report.Records.Add(MoveTemp(Record));
	}
	Report.ReturnedRecords = Report.Records.Num();
	if (bRecordExceededBudget && Report.ReturnedRecords == 0 && Offset < Snapshot.Records.Num())
	{
		Report.bOk = false;
		Report.NextCursor.Reset();
		Report.Status = TEXT("record_exceeds_output_budget");
		Report.Diagnostic = TEXT("The next base record cannot fit the requested output budget even after detail trimming.");
		return Report;
	}
	const int32 NextOffset = Offset + Report.ReturnedRecords;
	if (NextOffset < Snapshot.Records.Num())
	{
		Report.bTruncated = true;
		if (Snapshot.bComplete)
		{
			Report.NextCursor = TEXT("revision:") + Snapshot.Revision
				+ TEXT("|offset:") + FString::FromInt(NextOffset);
		}
	}
	if (Request.bIncludePrerequisites)
	{
		for (const auto& Status : FHyperAIStudioAnimationFacade::ResolveVariantStatuses())
		{
			const int32 Bytes = EstimateVariantBytes(Status);
			if (static_cast<int64>(EstimatedBytes) + Bytes > Request.MaxOutputBytes)
			{
				Report.bTruncated = true;
				break;
			}
			EstimatedBytes += Bytes;
			Report.Variants.Add(Status);
		}
	}
	Report.bOk = Snapshot.bComplete;
	Report.Status = Snapshot.bComplete ? TEXT("captured_loaded_state") : TEXT("captured_partial_state");
	Report.Diagnostic = Snapshot.bComplete
		? TEXT("Returned a deterministic page from a full bounded loaded-only Animation/Rigging capture.")
		: TEXT("Capture is partial; revision completeness is false and mutation CAS remains blocked.");
	return Report;
}

FHyperAIAnimationValidateReport UHyperAIStudioAnimationToolset::hyper_animation_validate(
	const FHyperAIAnimationValidateRequest& Request)
{
	using namespace HyperAIStudio::Animation::Private;
	FHyperAIAnimationValidateReport Report;
	if (Request.AssetPaths.Num() > FHyperAIStudioAnimationContracts::MaxAssetPaths
		|| Request.MaxIssues < 1 || Request.MaxIssues > FHyperAIStudioAnimationContracts::MaxIssues
		|| Request.MaxGameThreadMs < 1 || Request.MaxGameThreadMs > 250
		|| Request.MaxOutputBytes < MinOutputBytes
		|| Request.MaxOutputBytes > FHyperAIStudioAnimationContracts::MaxOutputBytes
		|| (!Request.ExpectedRevision.IsEmpty()
			&& !FHyperAIStudioAnimationContracts::IsCanonicalSha256(Request.ExpectedRevision)))
	{
		Report.Status = TEXT("invalid_request_bounds");
		Report.Diagnostic = TEXT("Animation validation request exceeds path, issue, output, or revision bounds.");
		return Report;
	}
	FHyperAIStudioAnimationValueSnapshot Snapshot;
	FString CaptureStatus;
	FString CaptureDiagnostic;
	if (!FHyperAIStudioAnimationFacade::CaptureLoaded(Request.AssetPaths, Request.Variant, true,
		Snapshot, CaptureStatus, CaptureDiagnostic, Request.MaxGameThreadMs))
	{
		Report.Status = CaptureStatus;
		Report.Diagnostic = Clip(CaptureDiagnostic);
		if (Request.bIncludePrerequisites)
		{
			int32 FailureBytes = 512;
			for (const auto& Status : FHyperAIStudioAnimationFacade::ResolveVariantStatuses())
			{
				const int32 Bytes = EstimateVariantBytes(Status);
				if (static_cast<int64>(FailureBytes) + Bytes > Request.MaxOutputBytes)
				{
					Report.bTruncated = true;
					break;
				}
				FailureBytes += Bytes;
				Report.Variants.Add(Status);
			}
		}
		return Report;
	}
	Report.bFreshCapture = true;
	Report.Revision = Snapshot.Revision;
	Report.bRevisionComplete = Snapshot.bComplete;
	bool bValidationTruncated = false;
	TArray<FHyperAIAnimationIssue> AllIssues =
		FHyperAIStudioAnimationContracts::ValidateValueSnapshot(
			Snapshot, Request.bRequirePackagesClean, Request.MaxIssues, bValidationTruncated);
	Report.bTruncated = bValidationTruncated;
	if (!Request.ExpectedRevision.IsEmpty() && Request.ExpectedRevision != Snapshot.Revision)
	{
		AddIssue(AllIssues, Request.MaxIssues, Report.bTruncated,
			TEXT("expected_revision_mismatch"), TEXT("error"), TEXT("snapshot"), FString(),
			TEXT("snapshot"), -1,
			TEXT("Fresh loaded-state revision no longer matches expected_revision."));
	}
	int32 EstimatedBytes = 1024;
	for (const auto& Issue : AllIssues)
	{
		if (Issue.Severity == TEXT("error")) ++Report.ErrorCount;
		else if (Issue.Severity == TEXT("warning")) ++Report.WarningCount;
		else ++Report.InfoCount;
		const int32 Bytes = EstimateIssueBytes(Issue);
		if (static_cast<int64>(EstimatedBytes) + Bytes > Request.MaxOutputBytes)
		{
			Report.bTruncated = true;
			continue;
		}
		EstimatedBytes += Bytes;
		Report.Issues.Add(Issue);
	}
	if (Request.bIncludePrerequisites)
	{
		for (const auto& Status : FHyperAIStudioAnimationFacade::ResolveVariantStatuses())
		{
			const int32 Bytes = EstimateVariantBytes(Status);
			if (static_cast<int64>(EstimatedBytes) + Bytes > Request.MaxOutputBytes)
			{
				Report.bTruncated = true;
				break;
			}
			EstimatedBytes += Bytes;
			Report.Variants.Add(Status);
		}
	}
	Report.bOk = true;
	Report.bValid = Snapshot.bComplete && Report.ErrorCount == 0 && !Report.bTruncated;
	Report.Status = Report.bValid ? TEXT("valid")
		: Report.bTruncated ? TEXT("validation_truncated") : TEXT("validation_failed");
	Report.Diagnostic = Report.bValid
		? TEXT("Fresh loaded-only Animation/Rigging state satisfies the independent bounded validator.")
		: TEXT("Fresh loaded-only state failed one or more validation, freshness, or completeness checks.");
	return Report;
}

FHyperAIAnimationApplyPlanReport UHyperAIStudioAnimationToolset::hyper_animation_apply_plan(
	const FHyperAIAnimationApplyPlanRequest& Request)
{
	return FHyperAIStudioAnimationContracts::BuildPlan(Request);
}

FString FHyperAIStudioAnimationTypedPayload::GetTypeId() const
{
	return FHyperAIStudioAnimationContracts::PayloadTypeId;
}

FString FHyperAIStudioAnimationTypedPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioAnimationContracts::PayloadSchemaFingerprint();
}

int32 FHyperAIStudioAnimationTypedPayload::GetBoundedByteSize() const
{
	int64 Bytes = 256 + 2ll * (BaseRevision.Len() + SemanticFingerprint.Len());
	for (const auto& Operation : Operations)
	{
		Bytes += 512 + 2ll * (Operation.Type.Len() + Operation.TargetPath.Len()
			+ Operation.StableId.Len()
			+ Operation.ExpectedRevision.Len() + Operation.ReferencePath.Len()
			+ Operation.Name.Len() + Operation.SecondaryName.Len()
			+ Operation.TransitionRule.Kind.Len() + Operation.TransitionRule.ParameterName.Len()
			+ Operation.Notify.Kind.Len() + Operation.Notify.Name.Len());
		for (const auto& Segment : Operation.Segments) Bytes += 96 + 2ll * Segment.SequencePath.Len();
		for (const auto& Sample : Operation.Samples) Bytes += 80 + 2ll * Sample.SequencePath.Len();
		for (const auto& Mapping : Operation.Mappings)
			Bytes += 48 + 2ll * (Mapping.Source.Len() + Mapping.Target.Len());
	}
	return static_cast<int32>(FMath::Min<int64>(Bytes, MAX_int32));
}

FString FHyperAIStudioAnimationTypedPayload::GetSemanticFingerprint() const
{
	return SemanticFingerprint;
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
FHyperAIStudioAnimationTypedPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioAnimationTypedPayload, ESPMode::ThreadSafe> Clone =
		MakeShared<FHyperAIStudioAnimationTypedPayload, ESPMode::ThreadSafe>();
	Clone->Operations = Operations;
	Clone->BaseRevision = BaseRevision;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioAnimationResultPayload::GetTypeId() const
{
	return FHyperAIStudioAnimationContracts::ResultTypeId;
}

FString FHyperAIStudioAnimationResultPayload::GetSchemaFingerprint() const
{
	return FHyperAIStudioAnimationContracts::ResultSchemaFingerprint();
}

int32 FHyperAIStudioAnimationResultPayload::GetBoundedByteSize() const
{
	return 96 + 2 * (Phase.Len() + Revision.Len());
}

FHyperAIStudioAnimationDomainAdapter::FHyperAIStudioAnimationDomainAdapter()
	: Descriptor(FHyperAIStudioAnimationContracts::GetBaseAdapterDescriptor())
{
}

const FHyperAIStudioDomainAdapterDescriptor&
FHyperAIStudioAnimationDomainAdapter::GetDescriptor() const
{
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioAnimationDomainAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	if (Context.Binding.PackId != Descriptor.PackId
		|| Context.Binding.ToolName != TEXT("hyper_animation_apply_plan")
		|| Context.Binding.VariantId != FHyperAIStudioAnimationContracts::MutationVariantId
		|| Context.Safety != EHyperAIStudioDomainSafety::Edit
		|| Payload.GetTypeId() != FHyperAIStudioAnimationContracts::PayloadTypeId
		|| Payload.GetSchemaFingerprint()
			!= FHyperAIStudioAnimationContracts::PayloadSchemaFingerprint())
	{
		Result.StatusCode = TEXT("typed_binding_mismatch");
		Result.Diagnostic = TEXT("Animation adapter rejected a non-exact pack, variant, safety, or DTO binding.");
		return Result;
	}
	Result.StatusCode = TEXT("staged_backend_required");
	Result.Diagnostic = TEXT("Animation adapter is side-effect-free until PlanExecutionService owns the live lease, journal, transaction, compile, save, validate, and fresh-verify lifecycle.");
	return Result;
}

bool FHyperAIStudioAnimationStagingService::Stage(
	const FHyperAIStudioAnimationStagedArtifact& Artifact,
	bool& bOutReplay,
	FString& OutError)
{
	using namespace HyperAIStudio::Animation::Private;
	bOutReplay = false;
	OutError.Reset();
	const int64 NowMs = NowMonotonicMs();
	if (Artifact.CanonicalProjectId.IsEmpty()
		|| !FHyperAIStudioExtensionRuntime::IsValidOperationId(Artifact.OperationId)
		|| !FHyperAIStudioAnimationContracts::IsCanonicalSha256(Artifact.StageId)
		|| !Artifact.Payload.IsValid()
		|| Artifact.Payload->GetTypeId() != FHyperAIStudioAnimationContracts::PayloadTypeId
		|| Artifact.Payload->GetSchemaFingerprint()
			!= FHyperAIStudioAnimationContracts::PayloadSchemaFingerprint()
		|| FHyperAIStudioAnimationContracts::ComputePayloadSemanticFingerprint(
			Artifact.Payload->Operations, Artifact.Payload->BaseRevision)
			!= Artifact.Payload->GetSemanticFingerprint()
		|| Artifact.Payload->GetSemanticFingerprint()
			!= Artifact.Prepared.Contract.ArtifactSemanticFingerprint
		|| Artifact.Prepared.Contract.ArtifactTypeId != Artifact.Payload->GetTypeId()
		|| Artifact.Prepared.Contract.ArtifactSchemaFingerprint
			!= Artifact.Payload->GetSchemaFingerprint()
		|| Artifact.Prepared.Contract.Binding.CanonicalProjectId != Artifact.CanonicalProjectId
		|| Artifact.Prepared.Contract.Binding.PackId != FHyperAIStudioAnimationContracts::PackId
		|| Artifact.Prepared.Contract.Binding.ToolName != TEXT("hyper_animation_apply_plan")
		|| Artifact.Prepared.Contract.Binding.VariantId
			!= FHyperAIStudioAnimationContracts::MutationVariantId
		|| Artifact.Prepared.Contract.Binding.ExpectedSafety != EHyperAIStudioDomainSafety::Edit
		|| Artifact.Prepared.Contract.Binding.ExpectedAdapterFingerprint
			!= FHyperAIStudioAnimationContracts::GetBaseAdapterDescriptor().AdapterFingerprint
		|| !FHyperAIStudioAnimationContracts::IsCanonicalSha256(Artifact.Prepared.PlanHash)
		|| !FHyperAIStudioAnimationContracts::IsCanonicalSha256(Artifact.Prepared.EffectFingerprint)
		|| Artifact.ExpiresMonotonicMs <= NowMs
		|| Artifact.ExpiresMonotonicMs > NowMs + FHyperAIStudioAnimationContracts::StageLifetimeMs + 1000)
	{
		OutError = TEXT("Staged Animation artifact identity, DTO, hashes, safety, or lifetime are invalid.");
		return false;
	}
	if (Artifact.StageId != FHyperAIStudioAnimationContracts::ComputeStageId(
		Artifact.CanonicalProjectId, Artifact.OperationId, Artifact.Prepared.PlanHash,
		Artifact.Prepared.EffectFingerprint))
	{
		OutError = TEXT("Staged Animation artifact has a non-exact stage identity.");
		return false;
	}
	FHyperAIStudioPreparedTypedArtifact Resealed;
	FString ResealError;
	if (!FHyperAIStudioTypedArtifactExecutor::Prepare(
		Artifact.Prepared.Contract, Resealed, ResealError)
		|| Resealed.ContractFingerprint != Artifact.Prepared.ContractFingerprint
		|| Resealed.BindingFingerprint != Artifact.Prepared.BindingFingerprint
		|| Resealed.CapabilityHash != Artifact.Prepared.CapabilityHash
		|| Resealed.PlanHash != Artifact.Prepared.PlanHash
		|| Resealed.AuthorizationPlanHash != Artifact.Prepared.AuthorizationPlanHash
		|| Resealed.EffectFingerprint != Artifact.Prepared.EffectFingerprint)
	{
		OutError = TEXT("Staged Animation artifact failed exact shared-contract resealing.");
		return false;
	}
	const TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> Detached =
		Artifact.Payload->CloneImmutable();
	if (&Detached.Get() == Artifact.Payload.Get()
		|| Detached->GetTypeId() != Artifact.Payload->GetTypeId()
		|| Detached->GetSchemaFingerprint() != Artifact.Payload->GetSchemaFingerprint()
		|| Detached->GetSemanticFingerprint() != Artifact.Payload->GetSemanticFingerprint()
		|| Detached->GetBoundedByteSize() != Artifact.Payload->GetBoundedByteSize()
		|| Detached->GetBoundedByteSize() <= 0
		|| Detached->GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
	{
		OutError = TEXT("Typed payload clone is aliased, drifted, empty, or outside the shared request bound.");
		return false;
	}
	const TSharedRef<const FHyperAIStudioAnimationTypedPayload, ESPMode::ThreadSafe> DetachedConcrete =
		StaticCastSharedRef<const FHyperAIStudioAnimationTypedPayload>(Detached);
	if (FHyperAIStudioAnimationContracts::ComputePayloadSemanticFingerprint(
		DetachedConcrete->Operations, DetachedConcrete->BaseRevision)
		!= DetachedConcrete->SemanticFingerprint)
	{
		OutError = TEXT("Detached typed payload failed concrete semantic recomputation.");
		return false;
	}
	FStagingState& State = GetStagingState();
	FScopeLock Lock(&State.Mutex);
	PruneExpired(State, NowMs);
	const FString Key = StageKey(Artifact.CanonicalProjectId, Artifact.OperationId);
	if (const FHyperAIStudioAnimationStagedArtifact* Existing = State.Artifacts.Find(Key))
	{
		if (Existing->Prepared.PlanHash != Artifact.Prepared.PlanHash
			|| Existing->Prepared.EffectFingerprint != Artifact.Prepared.EffectFingerprint
			|| Existing->Prepared.ContractFingerprint != Artifact.Prepared.ContractFingerprint
			|| Existing->Payload->GetSemanticFingerprint() != DetachedConcrete->GetSemanticFingerprint()
			|| Existing->StageId != Artifact.StageId)
		{
			OutError = TEXT("operation_id is already staged with a different exact plan/effect binding.");
			return false;
		}
		bOutReplay = true;
		return true;
	}
	if (State.Artifacts.Num() >= FHyperAIStudioTypedArtifactLimits::MaxStagedArtifacts)
	{
		OutError = TEXT("Bounded Animation stage store is full; wait for expiry or async-host integration.");
		return false;
	}
	FHyperAIStudioAnimationStagedArtifact Stored = Artifact;
	Stored.Payload = DetachedConcrete;
	State.Artifacts.Add(Key, MoveTemp(Stored));
	return true;
}

int32 FHyperAIStudioAnimationStagingService::NumStaged()
{
	using namespace HyperAIStudio::Animation::Private;
	FStagingState& State = GetStagingState();
	FScopeLock Lock(&State.Mutex);
	PruneExpired(State, NowMonotonicMs());
	return State.Artifacts.Num();
}

void FHyperAIStudioAnimationStagingService::Reset()
{
	using namespace HyperAIStudio::Animation::Private;
	FStagingState& State = GetStagingState();
	FScopeLock Lock(&State.Mutex);
	State.Artifacts.Reset();
}

void FHyperAIStudioAnimationRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
		this, &FHyperAIStudioAnimationRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioAnimationRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsRegistration && IsInGameThread() && UObjectInitialized()
		&& UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(
			UHyperAIStudioAnimationToolset::StaticClass(),
			FHyperAIStudioAnimationContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioAnimation, Warning,
				TEXT("Could not unregister the owned Animation/Rigging toolset: %s"), *Error);
		}
	}
	FHyperAIStudioAnimationStagingService::Reset();
	bOwnsRegistration = false;
	bStarted = false;
}

bool FHyperAIStudioAnimationRegistration::IsRegistered() const
{
	return UObjectInitialized() && UToolsetRegistry::IsAvailable()
		&& FHyperAIStudioAnimationContracts::IsRegistrationAllowed(
			FHyperAIStudioAnimationContracts::IsPendingTestRegistrationEnabled())
		&& FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
			UHyperAIStudioAnimationToolset::StaticClass(),
			FHyperAIStudioAnimationContracts::GetQualifiedToolsetName());
}

void FHyperAIStudioAnimationRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized()
		|| !UToolsetRegistry::IsAvailable()
		|| !FHyperAIStudioAnimationContracts::IsRegistrationAllowed(
			FHyperAIStudioAnimationContracts::IsPendingTestRegistrationEnabled())) return;
	if (!FHyperAIStudioCapabilityRuntimeIndex::IsOwned(
		UHyperAIStudioAnimationToolset::StaticClass(),
		FHyperAIStudioAnimationContracts::GetQualifiedToolsetName()))
	{
		FString Error;
		bOwnsRegistration = FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(
			UHyperAIStudioAnimationToolset::StaticClass(),
			FHyperAIStudioAnimationContracts::GetQualifiedToolsetName(), Error);
		if (!bOwnsRegistration)
		{
			UE_LOG(LogHyperAIStudioAnimation, Error,
				TEXT("ToolsetRegistry rejected the owned three-tool Animation/Rigging cohort: %s"),
				*Error);
		}
	}
}

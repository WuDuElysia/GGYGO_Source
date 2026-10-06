// Copyright Epic Games, Inc. All Rights Reserved.

#include "Animation/Nodes/GGYGOAnimNode_ActionPoseSlot.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimCompressionTypes.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/ScopeExit.h"
#if WITH_EDITOR
#include "Animation/AnimData/IAnimationDataModel.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOActionPose, Log, All);

namespace GGYGOActionPosePrivate
{
	// Source compression roundoff is not ancestor animation. This is a validation
	// tolerance in current mesh units; it does not clamp or replace authored body Z.
	constexpr double TranslationTolerance = 0.02;
	constexpr double ScaleTolerance = 0.0001;
	constexpr double RotationTolerance = 0.0001;

	bool UsableTransform(const FTransform& Transform)
	{
		const FVector Scale = Transform.GetScale3D();
		return !Transform.ContainsNaN() && Transform.IsRotationNormalized()
			&& FMath::Abs(Scale.X) > UE_SMALL_NUMBER && FMath::Abs(Scale.Y) > UE_SMALL_NUMBER
			&& FMath::Abs(Scale.Z) > UE_SMALL_NUMBER;
	}
}

void FGGYGOAnimNode_ActionPoseSlot::ClearEvaluationScratch()
{
	bInEvaluation = false;
	bSourceResidualReady = false;
	SourceResidualZ = 0.;
	EvaluationFailure.Reset();
}

void FGGYGOAnimNode_ActionPoseSlot::OnInitializeAnimInstance(
	const FAnimInstanceProxy* InProxy, const UAnimInstance* InAnimInstance)
{
	check(IsInGameThread());
	Super::OnInitializeAnimInstance(InProxy, InAnimInstance);
	// Native instance initialization runs on GT before NativeUpdate even when
	// root graph Initialize is deferred to the subsequent worker update.
	MontageCapabilityProvider.Reset();
	MontageCapabilityNativeUpdateCounter.Reset();
	bMontageCapabilityPublished = false;
	bMontageCapabilityValid = false;
	MontageCapabilityDiagnostic.Reset();
}

void FGGYGOAnimNode_ActionPoseSlot::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	ClearEvaluationScratch();
	bCapabilityReady = false;
	bHasConfigSnapshot = false;
	bHasReferenceSnapshot = false;
	Reference = FReferenceCache();
	// Preserve this native cycle's GT publication across deferred/relevancy graph
	// initialization. Its original provider and native traversal bound still apply.
	Super::Initialize_AnyThread(Context);
}

void FGGYGOAnimNode_ActionPoseSlot::PublishNativeMontageCapability(const UAnimInstance* OriginalProvider,
	const FGraphTraversalCounter& NativeUpdateCounter, bool bValid, const FString& Diagnostic)
{
	check(IsInGameThread());
	MontageCapabilityProvider = OriginalProvider;
	MontageCapabilityNativeUpdateCounter.SynchronizeWith(NativeUpdateCounter);
	bMontageCapabilityPublished = true;
	bMontageCapabilityValid = bValid;
	MontageCapabilityDiagnostic = Diagnostic;
}

bool FGGYGOAnimNode_ActionPoseSlot::BuildReferenceCache(const FBoneContainer& Bones,
	FReferenceCache& OutCache, FString& OutDiagnostic) const
{
	OutCache = FReferenceCache();
	OutDiagnostic.Reset();
	if (SlotName.IsNone() || BodyBoneName.IsNone() || TrajectoryBoneName.IsNone()
		|| BodyBoneName == TrajectoryBoneName || ComponentAlignment.ContainsNaN())
	{
		OutDiagnostic = TEXT("Explicit slot, distinct body/trajectory bones and finite component alignment are required.");
		return false;
	}
	if (!Bones.IsValid() || !Bones.GetSkeletalMeshAsset())
	{
		OutDiagnostic = TEXT("Current mesh RequiredBones/reference pose is unavailable; skeleton-only reference is not the contract.");
		return false;
	}
	OutCache.Mesh = Bones.GetSkeletalMeshAsset();
	const int32 BodyMeshIndex = Bones.GetPoseBoneIndexForBoneName(BodyBoneName);
	const int32 TrajectoryMeshIndex = Bones.GetPoseBoneIndexForBoneName(TrajectoryBoneName);
	if (BodyMeshIndex == INDEX_NONE || TrajectoryMeshIndex == INDEX_NONE)
	{
		OutDiagnostic = TEXT("Configured body or trajectory bone is absent from the current mesh.");
		return false;
	}
	OutCache.Body = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(BodyMeshIndex));
	OutCache.Trajectory = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(TrajectoryMeshIndex));
	if (OutCache.Body.GetInt() == INDEX_NONE || OutCache.Trajectory.GetInt() == INDEX_NONE)
	{
		OutDiagnostic = TEXT("Configured body or trajectory bone is not retained in the actual CompactPose/LOD RequiredBones.");
		return false;
	}
	if (Bones.BoneIsChildOf(OutCache.Body, OutCache.Trajectory)
		|| Bones.BoneIsChildOf(OutCache.Trajectory, OutCache.Body))
	{
		OutDiagnostic = TEXT("Body and trajectory bones must have independent branches; correcting one cannot move the other.");
		return false;
	}
	OutCache.BodyLocal = Bones.GetRefPoseTransform(OutCache.Body);
	OutCache.TrajectoryLocal = Bones.GetRefPoseTransform(OutCache.Trajectory);
	if (!GGYGOActionPosePrivate::UsableTransform(OutCache.BodyLocal)
		|| !GGYGOActionPosePrivate::UsableTransform(OutCache.TrajectoryLocal))
	{
		OutDiagnostic = TEXT("Current mesh body/trajectory reference transform is nonfinite, unnormalized or singular.");
		return false;
	}
	auto BuildParent = [&](FCompactPoseBoneIndex Bone, FTransform& OutParent) -> bool
	{
		OutParent = FTransform::Identity;
		for (FCompactPoseBoneIndex Parent = Bones.GetParentBoneIndex(Bone); Parent.GetInt() != INDEX_NONE;
			Parent = Bones.GetParentBoneIndex(Parent))
		{
			const FTransform& Local = Bones.GetRefPoseTransform(Parent);
			if (!GGYGOActionPosePrivate::UsableTransform(Local))
			{
				OutDiagnostic = TEXT("Current mesh reference ancestor is nonfinite, unnormalized or singular.");
				return false;
			}
			OutParent = OutParent * Local;
			if (!OutCache.Ancestors.ContainsByPredicate([Parent](const FReferenceAncestor& Entry) { return Entry.Index == Parent; }))
			{
				FReferenceAncestor& Entry = OutCache.Ancestors.AddDefaulted_GetRef();
				Entry.Name = Bones.GetReferenceSkeleton().GetBoneName(Bones.MakeMeshPoseIndex(Parent).GetInt());
				Entry.Index = Parent;
				Entry.Local = Local;
			}
		}
		return GGYGOActionPosePrivate::UsableTransform(OutParent);
	};
	if (!BuildParent(OutCache.Body, OutCache.BodyParent)
		|| !BuildParent(OutCache.Trajectory, OutCache.TrajectoryParent))
	{
		if (OutDiagnostic.IsEmpty())
		{
			OutDiagnostic = TEXT("Current mesh reference parent basis is singular or invalid.");
		}
		return false;
	}
	OutCache.ReferenceResidualZ = OutCache.BodyParent.TransformVector(OutCache.BodyLocal.GetTranslation()).Z
		- OutCache.TrajectoryParent.TransformVector(OutCache.TrajectoryLocal.GetTranslation()).Z;
	if (!FMath::IsFinite(OutCache.ReferenceResidualZ))
	{
		OutDiagnostic = TEXT("Current mesh reference body/trajectory residual is nonfinite.");
		return false;
	}
	return true;
}

bool FGGYGOAnimNode_ActionPoseSlot::SameReference(const FReferenceCache& A, const FReferenceCache& B)
{
	if (A.Mesh != B.Mesh || !A.BodyLocal.Equals(B.BodyLocal, UE_SMALL_NUMBER)
		|| !A.TrajectoryLocal.Equals(B.TrajectoryLocal, UE_SMALL_NUMBER)
		|| !A.BodyParent.Equals(B.BodyParent, UE_SMALL_NUMBER)
		|| !A.TrajectoryParent.Equals(B.TrajectoryParent, UE_SMALL_NUMBER)
		|| A.Ancestors.Num() != B.Ancestors.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < A.Ancestors.Num(); ++Index)
	{
		if (A.Ancestors[Index].Name != B.Ancestors[Index].Name
			|| !A.Ancestors[Index].Local.Equals(B.Ancestors[Index].Local, UE_SMALL_NUMBER))
		{
			return false;
		}
	}
	return true;
}

void FGGYGOAnimNode_ActionPoseSlot::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context)
{
	ClearEvaluationScratch();
	Super::CacheBones_AnyThread(Context);
	const FBoneContainer& Bones = Context.AnimInstanceProxy->GetRequiredBones();
	FReferenceCache NextReference;
	const bool bNextReady = BuildReferenceCache(Bones, NextReference, CapabilityDiagnostic);
	const bool bChanged = !bHasConfigSnapshot || CachedSlotName != SlotName || CachedBodyBoneName != BodyBoneName
		|| CachedTrajectoryBoneName != TrajectoryBoneName || CachedComponentAlignment != ComponentAlignment
		|| bCachedAlwaysUpdateSourcePose != bAlwaysUpdateSourcePose
		|| CachedMesh.Get() != Bones.GetSkeletalMeshAsset()
		|| (bNextReady && bHasReferenceSnapshot && !SameReference(Reference, NextReference));
	if (bChanged)
	{
		if (ContractConfigEpoch == MAX_uint64)
		{
			bSerialExhausted = true;
		}
		else
		{
			++ContractConfigEpoch;
		}
		// Keep an old valid reference through LOD-only missing-bone failures, but
		// discard it when the actual authored configuration/mesh changed.
		bHasReferenceSnapshot = false;
	}
	CachedSlotName = SlotName;
	CachedBodyBoneName = BodyBoneName;
	CachedTrajectoryBoneName = TrajectoryBoneName;
	CachedComponentAlignment = ComponentAlignment;
	bCachedAlwaysUpdateSourcePose = bAlwaysUpdateSourcePose;
	CachedMesh = Bones.GetSkeletalMeshAsset();
	bHasConfigSnapshot = true;
	if (bNextReady)
	{
		Reference = MoveTemp(NextReference);
		bHasReferenceSnapshot = true;
	}
	bCapabilityReady = bNextReady && !bSerialExhausted;
	if (!bCapabilityReady)
	{
		if (bSerialExhausted)
		{
			CapabilityDiagnostic = TEXT("Action pose contract identity counter exhausted; no lease may be issued.");
		}
		RecordFailure(Context.AnimInstanceProxy, CapabilityDiagnostic);
	}
}

bool FGGYGOAnimNode_ActionPoseSlot::IsConfigurationCurrent(const FBoneContainer& RequiredBones) const
{
	if (!bHasConfigSnapshot || CachedSlotName != SlotName || CachedBodyBoneName != BodyBoneName
		|| CachedTrajectoryBoneName != TrajectoryBoneName || CachedComponentAlignment != ComponentAlignment
		|| bCachedAlwaysUpdateSourcePose != bAlwaysUpdateSourcePose
		|| CachedMesh.Get() != RequiredBones.GetSkeletalMeshAsset())
	{
		return false;
	}
	FReferenceCache Current;
	FString Diagnostic;
	// Losing RequiredBones is a capability failure, not a logical configuration
	// replacement. A successful LOD rebuild compares reference values, not indices.
	return !BuildReferenceCache(RequiredBones, Current, Diagnostic)
		|| !bHasReferenceSnapshot || SameReference(Reference, Current);
}

bool FGGYGOAnimNode_ActionPoseSlot::CheckCapability(const FBoneContainer& RequiredBones, FString& OutDiagnostic) const
{
	FReferenceCache Current;
	if (!BuildReferenceCache(RequiredBones, Current, OutDiagnostic))
	{
		return false;
	}
	if (!IsConfigurationCurrent(RequiredBones) || !HasCurrentBoneIndices(RequiredBones)
		|| !bCapabilityReady || !bHasReferenceSnapshot
		|| bSerialExhausted || ContractConfigEpoch == 0)
	{
		OutDiagnostic = CapabilityDiagnostic.IsEmpty()
			? TEXT("Native CacheBones has not established the current action pose configuration.") : CapabilityDiagnostic;
		return false;
	}
	if (bFailureLatched)
	{
		OutDiagnostic = LastFailureDiagnostic;
		return false;
	}
	if (!bMontageCapabilityPublished || !bMontageCapabilityValid || !MontageCapabilityProvider.IsValid())
	{
		OutDiagnostic = MontageCapabilityDiagnostic.IsEmpty()
			? TEXT("Original provider has not published valid native montage input capability on its game-thread update.")
			: MontageCapabilityDiagnostic;
		return false;
	}
	OutDiagnostic.Reset();
	return true;
}

bool FGGYGOAnimNode_ActionPoseSlot::HasCurrentBoneIndices(const FBoneContainer& Bones) const
{
	if (!Bones.IsValid() || !bHasReferenceSnapshot || Bones.GetSkeletalMeshAsset() != Reference.Mesh.Get())
	{
		return false;
	}
	auto IsCurrent = [&](FName Name, FCompactPoseBoneIndex CachedIndex)
	{
		const int32 MeshIndex = Bones.GetPoseBoneIndexForBoneName(Name);
		return CachedIndex.GetInt() != INDEX_NONE && MeshIndex != INDEX_NONE
			&& Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshIndex)) == CachedIndex;
	};
	if (!IsCurrent(BodyBoneName, Reference.Body) || !IsCurrent(TrajectoryBoneName, Reference.Trajectory))
	{
		return false;
	}
	for (const FReferenceAncestor& Entry : Reference.Ancestors)
	{
		if (!IsCurrent(Entry.Name, Entry.Index)) { return false; }
	}
	return true;
}

bool FGGYGOAnimNode_ActionPoseSlot::ValidateMontageCapability(const UAnimMontage* Montage, FString& OutDiagnostic) const
{
	OutDiagnostic.Reset();
	const FAnimTrack* Track = Montage ? Montage->GetAnimationData(SlotName) : nullptr;
	if (!Track || Track->AnimSegments.IsEmpty() || Track->IsAdditive()
		|| Montage->BlendProfileIn || Montage->BlendProfileOut)
	{
		OutDiagnostic = TEXT("Action pose requires a nonempty nonadditive slot track with uniform montage blending.");
		return false;
	}
	for (const FAnimSegment& Segment : Track->AnimSegments)
	{
		const UAnimSequence* Sequence = Cast<UAnimSequence>(Segment.GetAnimReference());
		const USkeleton* Skeleton = Sequence ? Sequence->GetSkeleton() : nullptr;
		if (!Sequence || !Skeleton || Sequence->IsValidAdditive() || Sequence->HasRootMotion())
		{
			OutDiagnostic = FString::Printf(TEXT("Montage=%s Source=%s requires an ordinary nonadditive sequence without native root motion."),
				*GetPathNameSafe(Montage), *GetPathNameSafe(Sequence));
			return false;
		}
#if WITH_EDITOR
		// Authored editor tracks are checked explicitly. This is not a claim that
		// an editor compressed request selected the cooked compression backend.
		const IAnimationDataModel* Model = Sequence->GetDataModel();
		TArray<FName> TrackNames;
		if (Model)
		{
			Model->GetBoneTrackNames(TrackNames);
		}
		if (!Model || !TrackNames.Contains(BodyBoneName) || !TrackNames.Contains(TrajectoryBoneName))
		{
			OutDiagnostic = FString::Printf(TEXT("Montage=%s Source=%s has no authored body/trajectory track; reference-pose substitution is not allowed."),
				*GetPathNameSafe(Montage), *GetPathNameSafe(Sequence));
			return false;
		}
#else
		if (!Sequence->IsBoneCompressedDataValid())
		{
			OutDiagnostic = FString::Printf(TEXT("Montage=%s Source=%s compressed bone data is unavailable."),
				*GetPathNameSafe(Montage), *GetPathNameSafe(Sequence));
			return false;
		}
		const auto Compressed = Sequence->GetCompressedData();
		const int32 BodyIndex = Skeleton->GetReferenceSkeleton().FindBoneIndex(BodyBoneName);
		const int32 TrajectoryIndex = Skeleton->GetReferenceSkeleton().FindBoneIndex(TrajectoryBoneName);
		if (BodyIndex == INDEX_NONE || TrajectoryIndex == INDEX_NONE
			|| Compressed.Get().GetTrackIndexFromSkeletonIndex(BodyIndex) == INDEX_NONE
			|| Compressed.Get().GetTrackIndexFromSkeletonIndex(TrajectoryIndex) == INDEX_NONE)
		{
			OutDiagnostic = FString::Printf(TEXT("Montage=%s Source=%s compressed body/trajectory track is missing."),
				*GetPathNameSafe(Montage), *GetPathNameSafe(Sequence));
			return false;
		}
#endif
	}
	return true;
}

bool FGGYGOAnimNode_ActionPoseSlot::ValidateAncestors(const FCompactPose& Pose,
	double NativeCoefficientSum, FString& OutDiagnostic) const
{
	for (const FReferenceAncestor& Entry : Reference.Ancestors)
	{
		if (!Pose.IsValidIndex(Entry.Index))
		{
			OutDiagnostic = FString::Printf(TEXT("Ancestor=%s cached CompactPose index is unavailable."), *Entry.Name.ToString());
			return false;
		}
		const FTransform& Actual = Pose[Entry.Index];
		if (!GGYGOActionPosePrivate::UsableTransform(Actual)
			|| !Actual.GetTranslation().Equals(Entry.Local.GetTranslation() * NativeCoefficientSum, GGYGOActionPosePrivate::TranslationTolerance)
			|| !Actual.GetScale3D().Equals(Entry.Local.GetScale3D() * NativeCoefficientSum, GGYGOActionPosePrivate::ScaleTolerance)
			|| !Actual.GetRotation().Equals(Entry.Local.GetRotation(), GGYGOActionPosePrivate::RotationTolerance))
		{
			OutDiagnostic = FString::Printf(TEXT("Ancestor=%s does not satisfy the fixed current-mesh reference basis contract."), *Entry.Name.ToString());
			return false;
		}
	}
	return true;
}

double FGGYGOAnimNode_ActionPoseSlot::ReadResidualZ(const FCompactPose& Pose) const
{
	return Reference.BodyParent.TransformVector(Pose[Reference.Body].GetTranslation()).Z
		- Reference.TrajectoryParent.TransformVector(Pose[Reference.Trajectory].GetTranslation()).Z;
}

bool FGGYGOAnimNode_ActionPoseSlot::GetBodyParentTransform(const FCompactPose& Pose, FTransform& OutParent) const
{
	OutParent = FTransform::Identity;
	for (FCompactPoseBoneIndex Parent = Pose.GetBoneContainer().GetParentBoneIndex(Reference.Body);
		Parent.GetInt() != INDEX_NONE; Parent = Pose.GetBoneContainer().GetParentBoneIndex(Parent))
	{
		OutParent = OutParent * Pose[Parent];
	}
	return GGYGOActionPosePrivate::UsableTransform(OutParent);
}

void FGGYGOAnimNode_ActionPoseSlot::PostEvaluateSourcePose(FPoseContext& SourceContext)
{
	// Native Slot calls this hook even when SourceContext was not evaluated.
	if (!FMath::IsFinite(WeightData.SlotNodeWeight) || !FMath::IsFinite(WeightData.SourceWeight))
	{
		EvaluationFailure = TEXT("Native slot/source weights are nonfinite; an omitted Source cannot be read.");
		return;
	}
	if (!bInEvaluation || WeightData.SlotNodeWeight <= ZERO_ANIMWEIGHT_THRESH
		|| WeightData.SourceWeight <= ZERO_ANIMWEIGHT_THRESH || !bCapabilityReady)
	{
		return;
	}
	if (!HasCurrentBoneIndices(SourceContext.Pose.GetBoneContainer()))
	{
		EvaluationFailure = TEXT("Participating Source RequiredBones changed without the native CacheBones rebuild.");
		return;
	}
	if (!ValidateAncestors(SourceContext.Pose, 1., EvaluationFailure))
	{
		return;
	}
	SourceResidualZ = ReadResidualZ(SourceContext.Pose);
	bSourceResidualReady = FMath::IsFinite(SourceResidualZ);
	if (!bSourceResidualReady)
	{
		EvaluationFailure = TEXT("The participating native Source body/trajectory residual is nonfinite.");
	}
}

void FGGYGOAnimNode_ActionPoseSlot::RecordFailure(const FAnimInstanceProxy* Proxy, const FString& Reason)
{
	const FString Diagnostic = FString::Printf(TEXT("[Animation.ActionPose] AnimInstance=%s Mesh=%s Slot=%s Body=%s Trajectory=%s ConfigEpoch=%llu Reason=%s"),
		*GetPathNameSafe(Proxy ? Proxy->GetAnimInstanceObject() : nullptr), *GetPathNameSafe(CachedMesh.Get()),
		*SlotName.ToString(), *BodyBoneName.ToString(), *TrajectoryBoneName.ToString(), ContractConfigEpoch, *Reason);
	if (!bFailureLatched || LastFailureDiagnostic != Diagnostic)
	{
		if (FailureSerial == MAX_uint64)
		{
			bSerialExhausted = true;
			bCapabilityReady = false;
		}
		else
		{
			++FailureSerial;
		}
		LastFailureDiagnostic = Diagnostic;
	}
	bFailureLatched = true;
	if (LastLoggedFailure != Diagnostic)
	{
		UE_LOG(LogGGYGOActionPose, Error, TEXT("%s"), *Diagnostic);
		LastLoggedFailure = Diagnostic;
	}
}

void FGGYGOAnimNode_ActionPoseSlot::Evaluate_AnyThread(FPoseContext& Output)
{
	ClearEvaluationScratch();
	bInEvaluation = true;
	ON_SCOPE_EXIT { ClearEvaluationScratch(); };
	Super::Evaluate_AnyThread(Output);
	auto FailEvaluation = [&](const FString& Reason)
	{
		RecordFailure(Output.AnimInstanceProxy, Reason);
		// An engine-valid diagnostic pose accompanies a failed contract. This is
		// never a successful body correction or a replacement action/source.
		Output.ResetToRefPose();
	};
	if (!bCapabilityReady || !IsConfigurationCurrent(Output.Pose.GetBoneContainer())
		|| !HasCurrentBoneIndices(Output.Pose.GetBoneContainer()))
	{
		FailEvaluation(CapabilityDiagnostic.IsEmpty() ? TEXT("Current pose configuration is not ready or changed after CacheBones.") : CapabilityDiagnostic);
		return;
	}
	if (!FMath::IsFinite(WeightData.SlotNodeWeight) || !FMath::IsFinite(WeightData.SourceWeight)
		|| !FMath::IsFinite(WeightData.TotalNodeWeight))
	{
		FailEvaluation(TEXT("Native slot/source/total weights are nonfinite for the action pose contract."));
		return;
	}
	FVector TargetTranslation = ComponentAlignment;
	if (WeightData.SlotNodeWeight > ZERO_ANIMWEIGHT_THRESH)
	{
		if (WeightData.TotalNodeWeight <= ZERO_ANIMWEIGHT_THRESH)
		{
			FailEvaluation(TEXT("Native slot weights are invalid for the required action pose contract."));
			return;
		}
		if (!bMontageCapabilityPublished || !bMontageCapabilityValid
			|| MontageCapabilityProvider.Get() != Output.AnimInstanceProxy->GetAnimInstanceObject()
			// NativeUpdate publishes before Root Update increments this counter.
			// UE's check accepts exactly the same traversal or one native update ahead.
			|| !MontageCapabilityNativeUpdateCounter.WasSynchronizedCounter(Output.AnimInstanceProxy->GetUpdateCounter()))
		{
			FailEvaluation(MontageCapabilityDiagnostic.IsEmpty()
				? TEXT("Original game-thread montage input capability is missing, invalidated or outside its native update traversal.")
				: MontageCapabilityDiagnostic);
			return;
		}
		// Public native query only authenticates this round's existing WeightData.
		// These local values never replace weights, drive playback or mix a pose.
		float CurrentSlotWeight = 0.f;
		float CurrentSourceWeight = 0.f;
		float CurrentTotalWeight = 0.f;
		Output.AnimInstanceProxy->GetSlotWeight(SlotName, CurrentSlotWeight, CurrentSourceWeight, CurrentTotalWeight);
		if (CurrentSlotWeight != WeightData.SlotNodeWeight || CurrentSourceWeight != WeightData.SourceWeight
			|| CurrentTotalWeight != WeightData.TotalNodeWeight
			|| !FMath::IsNearlyEqual(WeightData.SourceWeight + WeightData.SlotNodeWeight, 1.f, UE_SMALL_NUMBER))
		{
			FailEvaluation(TEXT("Native montage input weights changed since Slot Update or contain an additive contribution; action pose correction is rejected."));
			return;
		}
		if (!EvaluationFailure.IsEmpty())
		{
			FailEvaluation(EvaluationFailure);
			return;
		}
		const double SourceCoefficient = WeightData.SourceWeight > ZERO_ANIMWEIGHT_THRESH
			? FMath::Clamp(static_cast<double>(WeightData.SourceWeight), 0., 1.) : 0.;
		const double NativeDivisor = WeightData.TotalNodeWeight > 1.f + ZERO_ANIMWEIGHT_THRESH
			? WeightData.TotalNodeWeight : 1.;
		const double ActionCoefficient = WeightData.TotalNodeWeight / NativeDivisor;
		FString Diagnostic;
		if ((SourceCoefficient > 0. && !bSourceResidualReady)
			|| !ValidateAncestors(Output.Pose, SourceCoefficient + ActionCoefficient, Diagnostic))
		{
			FailEvaluation(Diagnostic.IsEmpty() ? TEXT("The participating native Source residual was not produced in this Evaluate call.") : Diagnostic);
			return;
		}
		TargetTranslation.Z += ReadResidualZ(Output.Pose) - SourceCoefficient * SourceResidualZ
			- ActionCoefficient * Reference.ReferenceResidualZ;
	}
	FTransform BodyParent;
	if (TargetTranslation.ContainsNaN() || !GetBodyParentTransform(Output.Pose, BodyParent))
	{
		FailEvaluation(TEXT("Corrected component translation or actual body parent transform is invalid."));
		return;
	}
	const FVector LocalTranslation = BodyParent.InverseTransformPosition(TargetTranslation);
	if (LocalTranslation.ContainsNaN())
	{
		FailEvaluation(TEXT("Corrected body local translation is nonfinite."));
		return;
	}
	Output.Pose[Reference.Body].SetTranslation(LocalTranslation);
	bFailureLatched = false;
	LastLoggedFailure.Reset();
}

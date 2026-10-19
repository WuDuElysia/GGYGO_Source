/** @file GGYGOAnimNode_ActionPoseSlot.h @brief 原生 Slot 混合后的基础对齐与动作身体/轨迹分离。 */
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimNodes/AnimNode_Slot.h"
#include "BoneContainer.h"
#include "GGYGOAnimNode_ActionPoseSlot.generated.h"

class UAnimMontage;
class USkeletalMesh;
class UGGYGOMontageGuardAnimInstance;

/**
 * 原生 Slot 是唯一混合器；Source 的身体偏移随原生权重保留，动作部分分离身体/轨迹。
 * 不跨 Evaluate 保存姿态或 Montage 时间，不写 CMC；仅支持非 additive、均匀 Slot
 * 混合和固定参考祖先。Source 必须已将执行轨迹与身体姿态分离。
 */
USTRUCT(BlueprintInternalUseOnly)
struct GGYGO_API FGGYGOAnimNode_ActionPoseSlot : public FAnimNode_Slot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Action Pose", meta = (NeverAsPin))
	FName BodyBoneName = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Action Pose", meta = (NeverAsPin))
	FName TrajectoryBoneName = NAME_None;

	/** 基础对齐；保留 Source 的身体偏移及动作相对轨迹的身体偏移。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Action Pose", meta = (NeverAsPin))
	FVector ComponentAlignment = FVector::ZeroVector;

	virtual bool NeedsOnInitializeAnimInstance() const override { return true; }
	virtual void OnInitializeAnimInstance(const FAnimInstanceProxy* InProxy, const UAnimInstance* InAnimInstance) override;
	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override;
	virtual void Evaluate_AnyThread(FPoseContext& Output) override;

	/** Read only after the original AnimInstance's native GT evaluation barrier. */
	bool IsConfigurationCurrent(const FBoneContainer& RequiredBones) const;
	bool CheckCapability(const FBoneContainer& RequiredBones, FString& OutDiagnostic) const;
	bool ValidateMontageCapability(const UAnimMontage* Montage, FString& OutDiagnostic) const;
	uint64 GetContractConfigEpoch() const { return ContractConfigEpoch; }
	uint64 GetFailureSerial() const { return FailureSerial; }
	const FString& GetLastFailureDiagnostic() const { return LastFailureDiagnostic; }

protected:
	virtual void PostEvaluateSourcePose(FPoseContext& SourceContext) override;

private:
	// Only our GT provider publishes this derived capability. This does not grant
	// access to engine private/protected playback data or playback permissions.
	friend class UGGYGOMontageGuardAnimInstance;
	void PublishNativeMontageCapability(const UAnimInstance* OriginalProvider,
		const FGraphTraversalCounter& NativeUpdateCounter, bool bValid, const FString& Diagnostic);

	struct FReferenceAncestor
	{
		FName Name;
		FCompactPoseBoneIndex Index = FCompactPoseBoneIndex(INDEX_NONE);
		FTransform Local;
	};

	struct FReferenceCache
	{
		TWeakObjectPtr<USkeletalMesh> Mesh;
		FCompactPoseBoneIndex Body = FCompactPoseBoneIndex(INDEX_NONE);
		FCompactPoseBoneIndex Trajectory = FCompactPoseBoneIndex(INDEX_NONE);
		FTransform BodyLocal;
		FTransform TrajectoryLocal;
		FTransform BodyParent;
		FTransform TrajectoryParent;
		TArray<FReferenceAncestor> Ancestors;
		FVector ReferenceBodyTranslation = FVector::ZeroVector;
		FVector ReferenceResidual = FVector::ZeroVector;
	};

	bool BuildReferenceCache(const FBoneContainer& Bones, FReferenceCache& OutCache, FString& OutDiagnostic) const;
	static bool SameReference(const FReferenceCache& A, const FReferenceCache& B);
	bool HasCurrentBoneIndices(const FBoneContainer& Bones) const;
	bool ValidateAncestors(const FCompactPose& Pose, double NativeCoefficientSum, FString& OutDiagnostic) const;
	FVector ReadResidualTranslation(const FCompactPose& Pose) const;
	bool GetBodyParentTransform(const FCompactPose& Pose, FTransform& OutParent) const;
	void RecordFailure(const FAnimInstanceProxy* Proxy, const FString& Reason);
	void ClearEvaluationScratch();

	FReferenceCache Reference;
	FName CachedSlotName;
	FName CachedBodyBoneName;
	FName CachedTrajectoryBoneName;
	FVector CachedComponentAlignment = FVector::ZeroVector;
	TWeakObjectPtr<USkeletalMesh> CachedMesh;
	bool bHasConfigSnapshot = false;
	bool bCachedAlwaysUpdateSourcePose = false;
	bool bHasReferenceSnapshot = false;
	bool bCapabilityReady = false;
	bool bFailureLatched = false;
	bool bSerialExhausted = false;
	uint64 ContractConfigEpoch = 0;
	uint64 FailureSerial = 0;
	FString CapabilityDiagnostic;
	FString LastFailureDiagnostic;
	FString LastLoggedFailure;

	// GT validation of the existing native montage inputs. Contains no playback
	// identities, assets, poses, positions or copied weights. Expires at the next
	// native publication/instance initialization; the engine counter bounds reuse.
	TWeakObjectPtr<const UAnimInstance> MontageCapabilityProvider;
	FGraphTraversalCounter MontageCapabilityNativeUpdateCounter;
	bool bMontageCapabilityPublished = false;
	bool bMontageCapabilityValid = false;
	FString MontageCapabilityDiagnostic;

	// Borrowed from the native hook during this one Evaluate call; cleared at both boundaries.
	bool bInEvaluation = false;
	bool bSourceTranslationContributionReady = false;
	FVector SourceTranslationContribution = FVector::ZeroVector;
	FString EvaluationFailure;
};

/**
 * @file GGYGOAnimNode_BodyLean.h
 * @brief 通用整身倾身骨控制器；骨链及膝弯方向由 AnimBP 配置
 */
#pragma once

#include "CoreMinimal.h"
#include "BoneControllers/AnimNode_SkeletalControlBase.h"
#include "GGYGOAnimNode_BodyLean.generated.h"

USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOBodyLeanLegDefinition
{
	GENERATED_BODY()

	/** Foot 的两个直接父骨分别为膝和髋。 */
	UPROPERTY(EditAnywhere, Category = "Body Lean")
	FBoneReference Foot;

	/** 髋骨局部空间的单位膝弯方向；用于源姿态为直腿时确定求解平面。 */
	UPROPERTY(EditAnywhere, Category = "Body Lean")
	FVector KneeBendAxisLocal = FVector::ZeroVector;
};

/** 仅处理本次输入pose；满权重保持源脚目标，最终输出遵循UE原生Alpha混合。无跨帧脚接触缓存。 */
USTRUCT(BlueprintInternalUseOnly)
struct GGYGO_API FGGYGOAnimNode_BodyLean : public FAnimNode_SkeletalControlBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Body Lean", meta = (NeverAsPin))
	FBoneReference Body;

	UPROPERTY(EditAnywhere, Category = "Body Lean", meta = (NeverAsPin))
	FGGYGOBodyLeanLegDefinition LeftLeg;

	UPROPERTY(EditAnywhere, Category = "Body Lean", meta = (NeverAsPin))
	FGGYGOBodyLeanLegDefinition RightLeg;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body Lean", meta = (PinShownByDefault))
	float LeanDegrees = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body Lean", meta = (PinShownByDefault))
	FVector LeanAxisCS = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Body Lean", meta = (PinShownByDefault))
	FVector UpDirectionCS = FVector::ZeroVector;

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	virtual void GatherDebugData(FNodeDebugData& DebugData) override;

protected:
	virtual void InitializeBoneReferences(const FBoneContainer& RequiredBones) override;
	virtual bool IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones) override;
	virtual void EvaluateComponentPose_AnyThread(FComponentSpacePoseContext& Output) override;
	virtual void EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms) override;

private:
	struct FLegIndices
	{
		FCompactPoseBoneIndex Upper = FCompactPoseBoneIndex(INDEX_NONE);
		FCompactPoseBoneIndex Lower = FCompactPoseBoneIndex(INDEX_NONE);
		FCompactPoseBoneIndex Foot = FCompactPoseBoneIndex(INDEX_NONE);
	};
	FLegIndices Legs[2];
	FString BoneDiagnostic;
	FString LastLoggedDiagnostic;
	void RejectPose(FComponentSpacePoseContext& Output, const FString& Diagnostic);
};

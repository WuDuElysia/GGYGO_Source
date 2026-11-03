/**
 * @file GGYGOAnimNode_BodyLean.cpp
 * @brief 本次输入骨姿态的捕获、合法性校验和整身求解提交
 */
#include "Animation/Nodes/GGYGOAnimNode_BodyLean.h"

#include "Animation/Nodes/GGYGOBodyLeanEvaluation.h"
#include "Animation/AnimInstanceProxy.h"

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOBodyLean, Log, All);

void FGGYGOAnimNode_BodyLean::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	LastLoggedDiagnostic.Reset();
	// 状态重入可只调用 Initialize；同一 RequiredBones 的 CacheBones 会被引擎跳过。
	// 骨索引和对应诊断只在 InitializeBoneReferences 中一起重建。
	FAnimNode_SkeletalControlBase::Initialize_AnyThread(Context);
}

void FGGYGOAnimNode_BodyLean::InitializeBoneReferences(const FBoneContainer& RequiredBones)
{
	CachedBoneContainerSerialNumber = 0;
	Legs[0] = {};
	Legs[1] = {};
	BoneDiagnostic.Reset();
	if (!RequiredBones.IsValid() || RequiredBones.GetSerialNumber() == 0)
	{
		BoneDiagnostic = TEXT("本次RequiredBones尚未建立有效骨集合");
		return;
	}
	Body.Initialize(RequiredBones);
	LeftLeg.Foot.Initialize(RequiredBones);
	RightLeg.Foot.Initialize(RequiredBones);
	FLegIndices CandidateLegs[2];
	const FGGYGOBodyLeanLegDefinition* Definitions[2] = { &LeftLeg, &RightLeg };
	if (!Body.IsValidToEvaluate(RequiredBones))
	{
		BoneDiagnostic = FString::Printf(TEXT("Body骨[%s]不在本次RequiredBones内"), *Body.BoneName.ToString());
		return;
	}
	const FCompactPoseBoneIndex BodyIndex = Body.GetCompactPoseIndex(RequiredBones);
	for (int32 LegIndex = 0; LegIndex < 2; ++LegIndex)
	{
		FLegIndices& Indices = CandidateLegs[LegIndex];
		const FGGYGOBodyLeanLegDefinition& Definition = *Definitions[LegIndex];
		if (!Definition.Foot.IsValidToEvaluate(RequiredBones)
			|| Definition.KneeBendAxisLocal.ContainsNaN()
			|| !FMath::IsNearlyEqual(Definition.KneeBendAxisLocal.SizeSquared(), 1.0, 1.e-4))
		{
			BoneDiagnostic = FString::Printf(TEXT("Leg[%d]脚骨[%s]或明确膝弯单位轴无效"), LegIndex, *Definition.Foot.BoneName.ToString());
			return;
		}
		Indices.Foot = Definition.Foot.GetCompactPoseIndex(RequiredBones);
		Indices.Lower = RequiredBones.GetParentBoneIndex(Indices.Foot);
		if (Indices.Lower != INDEX_NONE)
		{
			Indices.Upper = RequiredBones.GetParentBoneIndex(Indices.Lower);
		}
		if (Indices.Upper == INDEX_NONE || Indices.Upper == BodyIndex)
		{
			BoneDiagnostic = FString::Printf(TEXT("Leg[%d]必须具有Body下的完整髋/膝/脚两骨链"), LegIndex);
			return;
		}
		FCompactPoseBoneIndex Parent = RequiredBones.GetParentBoneIndex(Indices.Upper);
		while (Parent != INDEX_NONE && Parent != BodyIndex)
		{
			Parent = RequiredBones.GetParentBoneIndex(Parent);
		}
		if (Parent != BodyIndex)
		{
			BoneDiagnostic = FString::Printf(TEXT("Leg[%d]髋骨不属于配置Body的子树"), LegIndex);
			return;
		}
	}
	const FCompactPoseBoneIndex LeftIndices[] = { CandidateLegs[0].Upper, CandidateLegs[0].Lower, CandidateLegs[0].Foot };
	const FCompactPoseBoneIndex RightIndices[] = { CandidateLegs[1].Upper, CandidateLegs[1].Lower, CandidateLegs[1].Foot };
	for (const FCompactPoseBoneIndex Left : LeftIndices)
	{
		for (const FCompactPoseBoneIndex Right : RightIndices)
		{
			if (Left == Right)
			{
				BoneDiagnostic = TEXT("双腿骨链不能重叠");
				return;
			}
		}
	}
	Legs[0] = CandidateLegs[0];
	Legs[1] = CandidateLegs[1];
	CachedBoneContainerSerialNumber = RequiredBones.GetSerialNumber();
}

bool FGGYGOAnimNode_BodyLean::IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones)
{
	// 完整父链、配置轴和双腿不重叠已在 CacheBones 校验；当前只核原生骨集合身份。
	return CachedBoneContainerSerialNumber != 0
		&& CachedBoneContainerSerialNumber == RequiredBones.GetSerialNumber()
		&& RequiredBones.IsValid() && BoneDiagnostic.IsEmpty() && Body.IsValidToEvaluate(RequiredBones)
		&& LeftLeg.Foot.IsValidToEvaluate(RequiredBones) && RightLeg.Foot.IsValidToEvaluate(RequiredBones);
}

void FGGYGOAnimNode_BodyLean::RejectPose(FComponentSpacePoseContext& Output, const FString& Diagnostic)
{
	if (LastLoggedDiagnostic != Diagnostic)
	{
		UE_LOG(LogGGYGOBodyLean, Error, TEXT("[Animation][BodyLean] Instance=%s Node=%d Body=%s LeftFoot=%s RightFoot=%s Reason=%s"),
			*GetPathNameSafe(Output.AnimInstanceProxy ? Output.AnimInstanceProxy->GetAnimInstanceObject() : nullptr),
			Output.GetCurrentNodeId(), *Body.BoneName.ToString(), *LeftLeg.Foot.BoneName.ToString(),
			*RightLeg.Foot.BoneName.ToString(), *Diagnostic);
		LastLoggedDiagnostic = Diagnostic;
	}
	Output.ResetToRefPose();
}

void FGGYGOAnimNode_BodyLean::EvaluateComponentPose_AnyThread(FComponentSpacePoseContext& Output)
{
	FAnimNode_SkeletalControlBase::EvaluateComponentPose_AnyThread(Output);
	if (FAnimWeight::IsRelevant(ActualAlpha) && !IsValidToEvaluate(nullptr, Output.Pose.GetPose().GetBoneContainer()))
	{
		RejectPose(Output, BoneDiagnostic.IsEmpty() ? FString(TEXT("本次RequiredBones身份失效")) : BoneDiagnostic);
	}
}

void FGGYGOAnimNode_BodyLean::EvaluateSkeletalControl_AnyThread(
	FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms)
{
	const FBoneContainer& Bones = Output.Pose.GetPose().GetBoneContainer();
	const FCompactPose& Pose = Output.Pose.GetPose();
	const FCompactPoseBoneIndex BodyIndex = Body.GetCompactPoseIndex(Bones);
	bool bPoseIndicesValid = Pose.IsValidIndex(BodyIndex);
	for (const FLegIndices& Indices : Legs)
	{
		bPoseIndicesValid = bPoseIndicesValid && Pose.IsValidIndex(Indices.Upper)
			&& Pose.IsValidIndex(Indices.Lower) && Pose.IsValidIndex(Indices.Foot);
	}
	if (!bPoseIndicesValid)
	{
		RejectPose(Output, TEXT("当前输入姿态不包含完整Body/髋/膝/脚索引"));
		return;
	}
	FGGYGOBodyLeanInput Input;
	Input.Body = Output.Pose.GetComponentSpaceTransform(BodyIndex);
	Input.LeanDegrees = LeanDegrees;
	Input.LeanAxisCS = LeanAxisCS;
	Input.UpDirectionCS = UpDirectionCS;
	const FGGYGOBodyLeanLegDefinition* Definitions[2] = { &LeftLeg, &RightLeg };
	for (int32 LegIndex = 0; LegIndex < 2; ++LegIndex)
	{
		FGGYGOBodyLeanLegPose& Leg = Input.Legs[LegIndex];
		Leg.Upper = Output.Pose.GetComponentSpaceTransform(Legs[LegIndex].Upper);
		Leg.Lower = Output.Pose.GetComponentSpaceTransform(Legs[LegIndex].Lower);
		Leg.Foot = Output.Pose.GetComponentSpaceTransform(Legs[LegIndex].Foot);
		Leg.KneeBendDirectionCS = Leg.Upper.GetRotation().RotateVector(Definitions[LegIndex]->KneeBendAxisLocal);
	}
	FGGYGOBodyLeanResult Result;
	FString Diagnostic;
	if (!FGGYGOBodyLeanEvaluation::Evaluate(Input, Result, Diagnostic))
	{
		RejectPose(Output, Diagnostic);
		return;
	}
	LastLoggedDiagnostic.Reset();
	OutBoneTransforms.Add(FBoneTransform(BodyIndex, Result.Body));
	for (int32 LegIndex = 0; LegIndex < 2; ++LegIndex)
	{
		OutBoneTransforms.Add(FBoneTransform(Legs[LegIndex].Upper, Result.Legs[LegIndex].Upper));
		OutBoneTransforms.Add(FBoneTransform(Legs[LegIndex].Lower, Result.Legs[LegIndex].Lower));
		OutBoneTransforms.Add(FBoneTransform(Legs[LegIndex].Foot, Result.Legs[LegIndex].Foot));
	}
	OutBoneTransforms.Sort([](const FBoneTransform& A, const FBoneTransform& B) { return A.BoneIndex < B.BoneIndex; });
}

void FGGYGOAnimNode_BodyLean::GatherDebugData(FNodeDebugData& DebugData)
{
	FString DebugLine = DebugData.GetNodeName(this);
	AddDebugNodeData(DebugLine);
	DebugLine += FString::Printf(TEXT(" Body=%s Lean=%.2f Failure=%s"), *Body.BoneName.ToString(), LeanDegrees, *LastLoggedDiagnostic);
	DebugData.AddDebugItem(DebugLine);
	ComponentPose.GatherDebugData(DebugData);
}

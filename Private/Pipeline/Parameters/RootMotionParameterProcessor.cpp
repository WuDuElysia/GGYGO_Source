/*
 * @file RootMotionParameterProcessor.cpp
 * @brief 五根动画曲线驱动参数处理器实现
 *
 * RM_PosX / RM_PosY 是动画局部空间的累计位置曲线，差分后得到本帧局部位移。
 * RM_Dist 是累计距离曲线，用于输出距离增量和一致性诊断，不参与位移叠加。
 * RM_Speed 是 cm/s 速度曲线，保留原始采样值，作为动画速度唯一主值；
 * AnimSpeed 仅作为当前 RM_Speed 安全采样值的外部兼容镜像，不是固定 Walk/Run 速度。
 * RM_Yaw 是可选的累计源旋转曲线，只描述 TurnBack 原始转身时序，
 * 不直接决定运行时最终目标角度。
 */
#include "Pipeline/Parameters/RootMotionParameterProcessor.h"
#include "Data/Runtime/RuntimeData.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"

namespace
{
	const FName CurveNamePosX(TEXT("RM_PosX"));
	const FName CurveNamePosY(TEXT("RM_PosY"));
	const FName CurveNameDistance(TEXT("RM_Dist"));
	const FName CurveNameSpeed(TEXT("RM_Speed"));
	const FName CurveNameYaw(TEXT("RM_Yaw"));

	float SanitizePositionSample(const float Value)
	{
		return FMath::IsFinite(Value) ? Value : 0.f;
	}

	float SanitizeNonNegativeSample(const float Value)
	{
		return FMath::IsFinite(Value) ? FMath::Max(Value, 0.f) : 0.f;
	}

	float SanitizeYawSample(const float Value)
	{
		return FMath::IsFinite(Value) ? Value : 0.f;
	}
}

void FRootMotionParameterProcessor::Init(USkeletalMeshComponent* InMesh)
{
	Mesh = InMesh;
	ResetSampleState();
}

void FRootMotionParameterProcessor::ResetSampleState()
{
	PreviousPosX = 0.f;
	PreviousPosY = 0.f;
	PreviousDist = 0.f;
	PreviousYaw = 0.f;
	bHasPreviousSample = false;
}

void FRootMotionParameterProcessor::Process(FRuntimeData& RuntimeData, float DeltaTime)
{
	// 先清零本帧输出，任何缺失上下文或退化帧都不会沿用上一帧位移/旋转增量。
	RuntimeData.RootMotion.AnimSpeed = 0.f;
	RuntimeData.RootMotion.RootMotionDelta = FVector::ZeroVector;
	RuntimeData.RootMotion.bHasRootMotion = false;
	RuntimeData.RootMotion.AnimCurveSpeed = 0.f;
	RuntimeData.RootMotion.AnimCurveDistanceDelta = 0.f;
	RuntimeData.RootMotion.AnimCurveAngle = 0.f;
	RuntimeData.RootMotion.AnimCurveYaw = 0.f;
	RuntimeData.RootMotion.AnimCurveYawDelta = 0.f;

	if (!Mesh)
	{
		ResetSampleState();
		return;
	}

	UAnimInstance* AnimInstance = Mesh->GetAnimInstance();
	if (!AnimInstance)
	{
		ResetSampleState();
		return;
	}

	const float CurrentPosX = SanitizePositionSample(AnimInstance->GetCurveValue(CurveNamePosX));
	const float CurrentPosY = SanitizePositionSample(AnimInstance->GetCurveValue(CurveNamePosY));
	const float CurrentDistance = SanitizeNonNegativeSample(AnimInstance->GetCurveValue(CurveNameDistance));
	const float CurrentSpeed = SanitizeNonNegativeSample(AnimInstance->GetCurveValue(CurveNameSpeed));
	const float CurrentYaw = SanitizeYawSample(AnimInstance->GetCurveValue(CurveNameYaw));

	// RM_Yaw 是累计源旋转值，即使没有上一帧基线，也可以安全暴露给 AnimBP 做姿势抵消。
	RuntimeData.RootMotion.AnimCurveYaw = CurrentYaw;

	const auto UpdateSampleState = [this, CurrentPosX, CurrentPosY, CurrentDistance, CurrentYaw]()
	{
		PreviousPosX = CurrentPosX;
		PreviousPosY = CurrentPosY;
		PreviousDist = CurrentDistance;
		PreviousYaw = CurrentYaw;
		bHasPreviousSample = true;
	};

	if (!FMath::IsFinite(DeltaTime) || DeltaTime <= KINDA_SMALL_NUMBER)
	{
		// 退化帧只更新基线，保持输出清零，避免除零或 NaN 传播。
		UpdateSampleState();
		return;
	}

	// RM_Speed 是 cm/s 的实际动画速度主值；AnimSpeed 仅镜像同一安全采样值供外部兼容使用。
	RuntimeData.RootMotion.AnimCurveSpeed = CurrentSpeed;
	RuntimeData.RootMotion.AnimSpeed = CurrentSpeed;

	if (!bHasPreviousSample)
	{
		// 首次采样只建立基线，不能把动画起始位置当成本帧位移；速度和 yaw 源值仍可读取。
		UpdateSampleState();
		return;
	}

	// 累计距离明显回退表示切换动画/新动画段；重置全部基线，避免产生反向大位移或大角度跳变。
	if (CurrentDistance < PreviousDist - KINDA_SMALL_NUMBER)
	{
		UpdateSampleState();
		return;
	}

	const FVector DeltaPos(
		CurrentPosX - PreviousPosX,
		CurrentPosY - PreviousPosY,
		0.f);
	if (!FMath::IsFinite(DeltaPos.X) || !FMath::IsFinite(DeltaPos.Y))
	{
		UpdateSampleState();
		return;
	}

	RuntimeData.RootMotion.AnimCurveDistanceDelta = FMath::Max(CurrentDistance - PreviousDist, 0.f);
	RuntimeData.RootMotion.AnimCurveYawDelta = FMath::FindDeltaAngleDegrees(PreviousYaw, CurrentYaw);

	if (!DeltaPos.IsNearlyZero())
	{
		RuntimeData.RootMotion.RootMotionDelta = DeltaPos;
		RuntimeData.RootMotion.bHasRootMotion = true;
		RuntimeData.RootMotion.AnimCurveAngle = FMath::RadiansToDegrees(
			FMath::Atan2(DeltaPos.Y, DeltaPos.X));
	}
	else
	{
		// 没有位置差分时不触发 Root Motion，AnimSpeed 和 AnimCurveYaw 仍保留当前安全采样值。
	}

	UpdateSampleState();
}

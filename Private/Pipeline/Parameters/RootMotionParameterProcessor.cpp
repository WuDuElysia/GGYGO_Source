/*
 * @file RootMotionParameterProcessor.cpp
 * @brief 动画曲线驱动参数处理器实现
 *
 * RM_PosX / RM_PosY 是固定动画起始根轨迹坐标中的累计位置曲线，差分后得到本帧固定坐标位移。
 * RM_Dist 是累计距离曲线，用于输出距离增量和一致性诊断，不参与位移叠加。
 * RM_Speed 是 cm/s 速度曲线，保留原始采样值，作为动画速度唯一主值；
 * RM_Yaw 是累计角度曲线，处理器用相邻采样值差分得到本帧角度增量，曲线值保持不变时输出 0；
 * AnimSpeed 仅作为当前 RM_Speed 安全采样值的外部兼容镜像，不是固定 Walk/Run 速度。
 * RM_VelocityDirX / RM_VelocityDirY 是同一根轨迹坐标中的每帧位移归一化方向；
 * authored 方向有效时优先使用，缺失或无效时回退到 RM_PosX/RM_PosY 差分。
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
	const FName CurveNameVelocityDirectionX(TEXT("RM_VelocityDirX"));
	const FName CurveNameVelocityDirectionY(TEXT("RM_VelocityDirY"));

	float SanitizePositionSample(const float Value)
	{
		return FMath::IsFinite(Value) ? Value : 0.f;
	}

	float SanitizeNonNegativeSample(const float Value)
	{
		return FMath::IsFinite(Value) ? FMath::Max(Value, 0.f) : 0.f;
	}

	float SanitizeDirectionSample(const float Value)
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
	RuntimeData.RootMotion.AnimCurveYawDelta = 0.f;
	RuntimeData.RootMotion.AnimCurveDistanceDelta = 0.f;
	RuntimeData.RootMotion.AnimCurveVelocity = FVector::ZeroVector;
	RuntimeData.RootMotion.AnimCurveVelocityDirection = FVector::ZeroVector;
	RuntimeData.RootMotion.bHasAuthoredVelocityDirection = false;
	RuntimeData.RootMotion.AnimCurveAngle = 0.f;
	RuntimeData.RootMotion.bHasRootMotionCurveSource = false;

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
	const float CurrentYawSample = SanitizeDirectionSample(
		AnimInstance->GetCurveValue(CurveNameYaw));
	const float CurrentVelocityDirectionX = SanitizeDirectionSample(
		AnimInstance->GetCurveValue(CurveNameVelocityDirectionX));
	const float CurrentVelocityDirectionY = SanitizeDirectionSample(
		AnimInstance->GetCurveValue(CurveNameVelocityDirectionY));
	const FVector AuthoredVelocityDirection(
		CurrentVelocityDirectionX,
		CurrentVelocityDirectionY,
		0.f);
	const bool bHasAuthoredVelocityDirection =
		!AuthoredVelocityDirection.IsNearlyZero(KINDA_SMALL_NUMBER);
	const FVector NormalizedAuthoredVelocityDirection =
		bHasAuthoredVelocityDirection
		? AuthoredVelocityDirection.GetSafeNormal2D()
		: FVector::ZeroVector;
	const bool bHasCurrentCurveMotionSource =
		CurrentSpeed > KINDA_SMALL_NUMBER
		|| !AuthoredVelocityDirection.IsNearlyZero(KINDA_SMALL_NUMBER)
		|| CurrentDistance > KINDA_SMALL_NUMBER;

	RuntimeData.RootMotion.bHasRootMotionCurveSource = bHasCurrentCurveMotionSource;

	const auto UpdateSampleState = [this,
		CurrentPosX,
		CurrentPosY,
		CurrentDistance,
		CurrentYawSample]()
	{
		PreviousPosX = CurrentPosX;
		PreviousPosY = CurrentPosY;
		PreviousDist = CurrentDistance;
		PreviousYaw = CurrentYawSample;
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
	// AnimCurveYawDelta 在建立上一帧基线并通过累计 RM_Yaw 差分后写入；首帧保持为 0。

	if (!bHasPreviousSample)
	{
		// 首次采样只建立位置/距离基线，不能把动画起始位置当成本帧位移；速度源仍可读取。
		UpdateSampleState();
		return;
	}

	// 累计距离明显回退表示切换动画/新动画段；重置基线，避免产生反向大位移。
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

	// RM_Yaw 是累计角度曲线，只把相邻采样值的变化量作为本帧增量；曲线保持不变时输出 0。
	const float YawDelta = CurrentYawSample - PreviousYaw;
	RuntimeData.RootMotion.AnimCurveYawDelta = FMath::IsFinite(YawDelta)
		? YawDelta
		: 0.f;

	RuntimeData.RootMotion.AnimCurveDistanceDelta = FMath::Max(CurrentDistance - PreviousDist, 0.f);

	const FVector PositionDeltaDirection = DeltaPos.GetSafeNormal2D();
	const FVector EffectiveVelocityDirection = bHasAuthoredVelocityDirection
		? NormalizedAuthoredVelocityDirection
		: PositionDeltaDirection;

	if (!DeltaPos.IsNearlyZero())
	{
		RuntimeData.RootMotion.RootMotionDelta = DeltaPos;
		RuntimeData.RootMotion.AnimCurveVelocity = DeltaPos / DeltaTime;
		RuntimeData.RootMotion.bHasRootMotion = true;
	}

	if (!EffectiveVelocityDirection.IsNearlyZero())
	{
		RuntimeData.RootMotion.AnimCurveVelocityDirection = EffectiveVelocityDirection;
		RuntimeData.RootMotion.bHasAuthoredVelocityDirection = bHasAuthoredVelocityDirection;
		RuntimeData.RootMotion.AnimCurveAngle = FMath::RadiansToDegrees(
			FMath::Atan2(EffectiveVelocityDirection.Y, EffectiveVelocityDirection.X));
	}
	else
	{
		// 没有 authored 方向和位置差分时不触发位移；AnimSpeed 仍保留当前安全采样值。
	}

	UpdateSampleState();
}

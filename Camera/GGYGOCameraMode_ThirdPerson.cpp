/**
 * @file GGYGOCameraMode_ThirdPerson.cpp
 * @brief 第三人称相机实现
 */
#include "Camera/GGYGOCameraMode_ThirdPerson.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCameraMode_ThirdPerson)

UGGYGOCameraMode_ThirdPerson::UGGYGOCameraMode_ThirdPerson()
{
	// 动作游戏的默认视野比射击游戏宽一些，便于看清周围的敌人位置。
	FieldOfView = 85.0f;

	// 俯仰上限收紧：完全俯视会让角色被自己遮住，完全仰视则看不到地面敌人。
	ViewPitchMin = -70.0f;
	ViewPitchMax = 70.0f;
}

FGGYGOCameraEvaluationResult UGGYGOCameraMode_ThirdPerson::ValidateModeConfiguration() const
{
	if (!FMath::IsFinite(TargetOffset.X))
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("TargetOffset.X"), TEXT("non-finite"));
	}
	if (!FMath::IsFinite(TargetOffset.Y))
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("TargetOffset.Y"), TEXT("non-finite"));
	}
	if (!FMath::IsFinite(TargetOffset.Z))
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("TargetOffset.Z"), TEXT("non-finite"));
	}
	// Disabled prevention intentionally does not use either collision parameter.
	if (bPreventPenetration)
	{
		if (!FMath::IsFinite(PenetrationProbeRadius) || PenetrationProbeRadius < 0.0f)
		{
			return FGGYGOCameraEvaluationResult::Failure(this, TEXT("PenetrationProbeRadius"),
				FMath::IsFinite(PenetrationProbeRadius) ? TEXT("negative") : TEXT("non-finite"));
		}
		if (!FMath::IsFinite(PenetrationRecoverySpeed) || PenetrationRecoverySpeed < 0.0f)
		{
			return FGGYGOCameraEvaluationResult::Failure(this, TEXT("PenetrationRecoverySpeed"),
				FMath::IsFinite(PenetrationRecoverySpeed) ? TEXT("negative") : TEXT("non-finite"));
		}
	}
	return FGGYGOCameraEvaluationResult::Success();
}

void UGGYGOCameraMode_ThirdPerson::UpdateView(float)
{
	const FVector PivotLocation = GetPivotLocation();
	FRotator PivotRotation = GetPivotRotation();

	PivotRotation.Pitch = FMath::ClampAngle(PivotRotation.Pitch, ViewPitchMin, ViewPitchMax);

	View.Rotation = PivotRotation;
	View.ControlRotation = View.Rotation;
	View.FieldOfView = FieldOfView;

	// 偏移在**视线空间**而不是角色局部空间里应用：镜头应当始终在视线后方，
	// 与角色自身朝向无关。用角色朝向会让原地转身时镜头绕着角色转。
	const FVector DesiredOffset = PivotRotation.RotateVector(TargetOffset);
	const FVector DesiredLocation = PivotLocation + DesiredOffset;
	View.Location = DesiredLocation;

	CameraPenetrationRequest.bEnabled = bPreventPenetration;
	if (bPreventPenetration)
	{
		CameraPenetrationRequest.PivotLocation = PivotLocation;
		CameraPenetrationRequest.ProbeRadius = PenetrationProbeRadius;
		CameraPenetrationRequest.RecoverySpeed = PenetrationRecoverySpeed;
	}
}

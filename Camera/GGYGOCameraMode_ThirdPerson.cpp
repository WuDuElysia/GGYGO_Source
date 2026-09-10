/**
 * @file GGYGOCameraMode_ThirdPerson.cpp
 * @brief 第三人称相机实现
 */
#include "Camera/GGYGOCameraMode_ThirdPerson.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCameraMode_ThirdPerson)

UGGYGOCameraMode_ThirdPerson::UGGYGOCameraMode_ThirdPerson()
{
	// 动作游戏的默认视野比射击游戏宽一些，便于看清周围的敌人位置。
	FieldOfView = 85.0f;

	// 俯仰上限收紧：完全俯视会让角色被自己遮住，完全仰视则看不到地面敌人。
	ViewPitchMin = -70.0f;
	ViewPitchMax = 70.0f;
}

void UGGYGOCameraMode_ThirdPerson::UpdateView(float DeltaTime)
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

	if (!bPreventPenetration)
	{
		View.Location = DesiredLocation;
		CurrentArmLengthRatio = 1.0f;
		return;
	}

	const AActor* TargetActor = GetTargetActor();
	const UWorld* World = GetWorld();
	if (!TargetActor || !World)
	{
		View.Location = DesiredLocation;
		return;
	}

	// 从枢轴往目标位置扫一个球，命中就说明中间有遮挡。
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(GGYGOCameraPenetration), /*bTraceComplex=*/false);
	QueryParams.AddIgnoredActor(TargetActor);

	FHitResult Hit;
	const bool bBlocked = World->SweepSingleByChannel(
		Hit,
		PivotLocation,
		DesiredLocation,
		FQuat::Identity,
		ECC_Camera,
		FCollisionShape::MakeSphere(PenetrationProbeRadius),
		QueryParams);

	float TargetRatio = 1.0f;
	if (bBlocked)
	{
		TargetRatio = FMath::Clamp(Hit.Time, 0.0f, 1.0f);
	}

	if (TargetRatio < CurrentArmLengthRatio)
	{
		// 立即拉近。平滑拉近意味着这几帧里镜头仍在墙内，会看到背面。
		CurrentArmLengthRatio = TargetRatio;
	}
	else
	{
		// 平滑推远。立即推远会在经过门框、柱子时产生剧烈的前后抽动。
		CurrentArmLengthRatio = FMath::FInterpTo(CurrentArmLengthRatio, TargetRatio, DeltaTime, PenetrationRecoverySpeed);
	}

	View.Location = PivotLocation + (DesiredOffset * CurrentArmLengthRatio);
}

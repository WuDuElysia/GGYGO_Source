/**
 * @file MovementParameterProcessor.cpp
 * @brief 移动参数处理器实现
 */
#include "Pipeline/Parameters/MovementParameterProcessor.h"
#include "Data/Runtime/RuntimeData.h"
#include "GameFramework/Character.h"

void FMovementParameterProcessor::Init(ACharacter* InOwner)
{
	Owner = InOwner;
}

void FMovementParameterProcessor::Process(FRuntimeData& RuntimeData, float DeltaTime)
{
	float TargetX = 0.f;
	float TargetY = 0.f;

	if (!RuntimeData.Intent.DesiredWorldMoveDir.IsNearlyZero())
	{
		// 动画移动方向必须相对 Actor 当前水平朝向计算；TurnBack Frozen 阶段因此能读到后向输入。
		const float ReferenceYaw = Owner
			? Owner->GetActorRotation().Yaw
			: RuntimeData.View.ControlRotation.Yaw;
		const FRotator ActorYaw(0.f, ReferenceYaw, 0.f);
		const FVector LocalDir = ActorYaw.UnrotateVector(
			RuntimeData.Intent.DesiredWorldMoveDir.GetSafeNormal2D());

		// BlendSpace X 表示左右，Y 表示前后。
		TargetX = LocalDir.Y;
		TargetY = LocalDir.X;

		// 计算角度（-180 ~ 180）。
		RuntimeData.Movement.MoveAngle = FMath::RadiansToDegrees(
			FMath::Atan2(LocalDir.Y, LocalDir.X));
	}
	else
	{
		RuntimeData.Movement.MoveAngle = 0.f;
	}

	const float SafeDeltaTime = FMath::IsFinite(DeltaTime) && DeltaTime > 0.f
		? DeltaTime
		: 0.f;

	// FInterpTo 平滑插值，避免动画 BlendSpace 方向突变。
	SmoothedBlendX = FMath::FInterpTo(SmoothedBlendX, TargetX, SafeDeltaTime, SmoothSpeed);
	SmoothedBlendY = FMath::FInterpTo(SmoothedBlendY, TargetY, SafeDeltaTime, SmoothSpeed);

	// 这是动画层可读取的唯一 XY 投影；处理器私有平滑值不再停留在本地。
	RuntimeData.ZZZAnim.AnimBlendX = SmoothedBlendX;
	RuntimeData.ZZZAnim.AnimBlendY = SmoothedBlendY;
}

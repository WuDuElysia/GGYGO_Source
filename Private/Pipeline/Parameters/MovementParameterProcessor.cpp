/**
 * @file MovementParameterProcessor.cpp
 * @brief 移动参数处理器实现
 */
#include "Pipeline/Parameters/MovementParameterProcessor.h"
#include "Data/RuntimeData.h"

void FMovementParameterProcessor::Process(FRuntimeData& RuntimeData, float DeltaTime)
{
	float TargetX = 0.f;
	float TargetY = 0.f;

	if (!RuntimeData.DesiredWorldMoveDir.IsNearlyZero())
	{
		// 计算角色本地空间的移动角度
		FRotator CharRot(0.f, RuntimeData.ControlRotation.Yaw, 0.f);
		FVector LocalDir = CharRot.UnrotateVector(RuntimeData.DesiredWorldMoveDir);

		// 分解为 X（左右）和 Y（前后）分量
		TargetX = LocalDir.Y;
		TargetY = LocalDir.X;

		// 计算角度（-180 ~ 180）
		RuntimeData.MoveAngle = FMath::Atan2(LocalDir.Y, LocalDir.X) * (180.f / PI);
	}
	else
	{
		RuntimeData.MoveAngle = 0.f;
	}

	// FInterpTo 平滑插值，避免动画突变
	SmoothedBlendX = FMath::FInterpTo(SmoothedBlendX, TargetX, DeltaTime, SmoothSpeed);
	SmoothedBlendY = FMath::FInterpTo(SmoothedBlendY, TargetY, DeltaTime, SmoothSpeed);

	RuntimeData.AnimBlendX = SmoothedBlendX;
	RuntimeData.AnimBlendY = SmoothedBlendY;
}

/**
 * @file LocomotionIntentProcessor.cpp
 * @brief 移动意图处理器实现
 */
#include "Pipeline/Intents/LocomotionIntentProcessor.h"
#include "Data/InputData.h"
#include "Data/RuntimeData.h"

void FLocomotionIntentProcessor::Process(const FInputData& InputData, FRuntimeData& RuntimeData)
{
	const FVector2D& MoveInput = InputData.CurrentFrame.Move;

	if (MoveInput.IsNearlyZero())
	{
		RuntimeData.DesiredWorldMoveDir = FVector::ZeroVector;
		return;
	}

	// 从 ControlRotation 提取水平朝向（ViewRotationProcessor 已写入）
	FRotator YawRot(0.f, RuntimeData.ControlRotation.Yaw, 0.f);
	FVector Forward = FRotationMatrix(YawRot).GetUnitAxis(EAxis::X);
	FVector Right = FRotationMatrix(YawRot).GetUnitAxis(EAxis::Y);

	// 输入空间 → 世界空间（Z 归零保证水平方向）
	FVector WorldDir = (Forward * MoveInput.Y + Right * MoveInput.X);
	WorldDir.Z = 0.f;
	RuntimeData.DesiredWorldMoveDir = WorldDir.GetSafeNormal();

	// 速度档位
	RuntimeData.bWantsToSprint = InputData.CurrentFrame.bSprintHeld;
}

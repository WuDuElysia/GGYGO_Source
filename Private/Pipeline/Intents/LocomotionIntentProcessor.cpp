/**
 * @file LocomotionIntentProcessor.cpp
 * @brief 移动方向意图处理器实现
 *
 * 本处理器只负责两项职责：
 *   1. 将 2D 输入方向结合 RuntimeData.View.ControlRotation 转换为世界空间的 RuntimeData.Intent.DesiredWorldMoveDir；
 *   2. 将本帧是否存在有效移动方向同步到 RuntimeData.ZZZAnim.bShouldMove。
 *
 * 步态由 Gait_Authority 在独立阶段统一解析。本处理器不读取速度、输入
 * 修饰键或摇杆幅度来推断步态，也不写入任何步态通道或触发字段。
 * 依赖 ViewRotationProcessor 先写入 ControlRotation。
 */
#include "Pipeline/Intents/LocomotionIntentProcessor.h"
#include "Data/Input/InputData.h"
#include "Data/Runtime/RuntimeData.h"

void FLocomotionIntentProcessor::Process(const FInputData& InputData, FRuntimeData& RuntimeData)
{
	// 读取本帧摇杆输入（2D 向量，X=右，Y=前，范围 [-1, 1]）。
	const FVector2D& MoveInput = InputData.CurrentFrame.Move;

	// 无方向输入时清零期望方向，并同步动画移动意图。
	if (MoveInput.IsNearlyZero())
	{
		RuntimeData.Intent.DesiredWorldMoveDir = FVector::ZeroVector;
		RuntimeData.ZZZAnim.bShouldMove = false;
		return;
	}

	// 输入空间 → 世界空间转换。
	// 摇杆输入是相对摄像机的局部空间，需要使用水平朝向旋转到世界空间。
	const FRotator YawRot(0.f, RuntimeData.View.ControlRotation.Yaw, 0.f);
	const FRotationMatrix YawMatrix(YawRot);
	const FVector Forward = YawMatrix.GetUnitAxis(EAxis::X);
	const FVector Right = YawMatrix.GetUnitAxis(EAxis::Y);

	// 组合输入方向并强制保持水平，避免摄像机俯仰引入垂直移动分量。
	FVector WorldDir = Forward * MoveInput.Y + Right * MoveInput.X;
	WorldDir.Z = 0.f;
	RuntimeData.Intent.DesiredWorldMoveDir = WorldDir.GetSafeNormal();

	// 动画移动意图与方向处理在同一阶段同步。
	RuntimeData.ZZZAnim.bShouldMove = !RuntimeData.Intent.DesiredWorldMoveDir.IsNearlyZero();
}

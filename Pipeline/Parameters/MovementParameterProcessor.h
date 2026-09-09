/**
 * @file MovementParameterProcessor.h
 * @brief 移动参数处理器
 *
 * DesiredWorldMoveDir → 本地角度 MoveAngle → FInterpTo 平滑 → AnimBlendX/Y。
 * 依赖 LocomotionIntentProcessor 和 ViewRotationProcessor 先写入数据。
 */
#pragma once

#include "Pipeline/Interfaces/IParameterProcessor.h"

class ACharacter;

class FMovementParameterProcessor : public IParameterProcessor
{
public:
	/** 注入角色朝向，用于计算相对 Actor 的动画移动 XY。 */
	void Init(ACharacter* InOwner);

	virtual void Process(FRuntimeData& RuntimeData, float DeltaTime) override;

private:
	/** 所属角色；非拥有，仅用于读取当前水平朝向。 */
	ACharacter* Owner = nullptr;

	/** 平滑插值速度（越大越快到达目标值） */
	float SmoothSpeed = 8.0f;

	/** 当前平滑后的混合值（跨帧保持状态） */
	float SmoothedBlendX = 0.f;
	float SmoothedBlendY = 0.f;
};

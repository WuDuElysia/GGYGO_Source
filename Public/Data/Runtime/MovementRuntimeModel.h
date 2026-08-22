/**
 * @file MovementRuntimeModel.h
 * @brief 角色实际移动结果与 TurnBack 运行时模型
 */
#pragma once

#include "CoreMinimal.h"
#include "StateMachine/CharacterStateType.h"

/** TurnBack 相位及其 Frozen 阶段的方向契约。 */
struct FTurnBackRuntimeModel
{
	/** 急停转身的唯一逻辑相位。 */
	ETurnBackPhase Phase = ETurnBackPhase::None;

	/** Frozen 阶段保持的进入转身前世界方向。 */
	FVector EntryDirection = FVector::ZeroVector;
};

/** MotionDriver 产生的实际移动结果，以及 TurnBack 运行时状态。 */
struct FMovementRuntimeModel
{
	FTurnBackRuntimeModel TurnBack;

	/** 角色当前水平速度。 */
	float CurrentSpeed = 0.f;

	/** 相对于角色朝向的最终实际移动角度。 */
	float MoveAngle = 0.f;

	/** 根据实际速度判断的移动状态。 */
	bool bIsMoving = false;

	/** 物理/角色层提供的地面状态；当前保留原有数据契约。 */
	bool bIsGrounded = true;
};

/**
 * @file MovementRuntimeModel.h
 * @brief 角色实际移动结果与 TurnBack 运行时模型
 */
#pragma once

#include "CoreMinimal.h"
#include "StateMachine/CharacterStateType.h"

/** TurnBack 相位运行时模型；生命周期和第二段标记由逻辑层时间轴维护。 */
struct FTurnBackRuntimeModel
{
	/** 急停转身的唯一逻辑相位。 */
	ETurnBackPhase Phase = ETurnBackPhase::None;

	/** CanYaw AnimNotify 是否已被逻辑阶段消费；true 表示从此允许输入接管朝向和 d1 位移。 */
	bool bCanYaw = false;

	/** 是否已经进入逻辑第二段；与 bCanYaw 同步产生，但作为 MotionDriver/AnimBP 的 d1 路由标记独立保留。 */
	bool bSecondSegment = false;

	/** 从本次 TurnBack 进入 Frozen 起累计的逻辑时间（秒），仅作为诊断和状态权威。 */
	float ElapsedSeconds = 0.f;
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

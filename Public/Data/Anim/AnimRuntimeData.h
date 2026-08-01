/**
 * @file AnimRuntimeData.h
 * @brief 动画层 - 动画运行时数据
 *
 * 路径: Data/Anim/
 *
 * 逻辑管线每帧写入 → ZZZAnim / NTEAnim 决策层读取。
 * 当前仅填充 ZZZAnim Locomotion 所需字段，按需扩展。
 */
#pragma once

#include "StateMachine/CharacterStateType.h"  // EMovementGait

/**
 * 动画运行时数据（纯 C++ 结构体，无反射）
 *
 * 逻辑管线每帧写入 → ZZZAnimSnapshotCapture 只读抓取 → 快照 → AnimBP 过渡函数。
 * 仅 ZZZAnim Locomotion 使用，按需扩展。
 */
struct FAnimRuntimeData
{
	/** 步态（Walk / Run / Sprint），LocomotionIntentProcessor 写入 */
	EMovementGait Gait = EMovementGait::None;

	/** 有移动输入 / 应移动，意图管线写入 */
	bool bShouldMove = false;

	/** 本帧是否触发冲刺分流，由 LocomotionIntentProcessor 写入并由 Conduit→Moving 消费 */
	bool bSprintTrigger = false;

	/** 当前角色状态，StateManager 在更新逻辑状态时同步写入 */
	ECharacterStateType CurrentState = ECharacterStateType::Idle;

	/** 当前速度标量（cm/s），MotionDriver 写入 */
	float VelocityLength = 0.f;

	/** 2D 速度（cm/s），含方向 */
	float Velocity2DLength = 0.f;

	/** 输入方向角度（度） */
	float LastInputDirectionAngle = 0.f;
};

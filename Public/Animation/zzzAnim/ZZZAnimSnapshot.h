/**
 * @file ZZZAnimSnapshot.h
 * @brief ZZZ 动画快照结构体
 *
 * 游戏线程从 FAnimRuntimeData + Owner 抓取，
 * worker 线程与决策函数读取此快照；一次性过渡触发开关由对应决策消费。
 *
 * 需要捕获的字段在此结构体中声明，
 * 捕获实现在 CombatSnapshotCapture.cpp 中。
 */
#pragma once

#include "CoreMinimal.h"
#include "StateMachine/CharacterStateType.h"  // EMovementGait

struct FZZZAnimSnapshot
{
	// ---- Locomotion ----

	/** 当前步态（Walk/Run/Sprint）← AnimRuntimeData.NTE.Gait */
	EMovementGait Gait = EMovementGait::None;

	/** 是否有移动输入/意图 ← AnimRuntimeData.bShouldMove */
	bool bShouldMove = false;

	/** 本帧是否触发冲刺分流 ← AnimRuntimeData.bSprintTrigger */
	bool bSprintTrigger = false;

	/** 当前角色状态 ← AnimRuntimeData.CurrentState */
	ECharacterStateType CurrentState = ECharacterStateType::Idle;

	/** 当前速度标量（cm/s）← AnimRuntimeData.VelocityLength */
	float VelocityLength = 0.f;

	// ---- 通用 ----

	/** 是否在地面 ← ABaseCharacter::IsGrounded() */
	bool bGrounded = true;

	/** 是否被仲裁阻止移动 ← 暂无来源，恒 false */
	bool bBlockMove = false;
};

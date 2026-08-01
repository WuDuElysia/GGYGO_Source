/**
 * @file RuntimeData.h
 * @brief 逻辑层 - 运行时黑板：全局帧级共享数据中枢
 * 
 * 所有子系统通过 RuntimeData 共享数据，不直接互相引用。
 * 每个字段只有一个系统写入，多个系统读取。
 * 帧级意图在帧末由 ResetFrameIntents() 清零。
 *
 * 路径: Data/Logic/
 */

#pragma once

#include "CoreMinimal.h"
#include "StateMachine/CharacterStateType.h"
#include "Data/Anim/AnimRuntimeData.h"   // FAnimRuntimeData 子结构

/**
 * 运行时黑板 — 管线中所有系统的数据交换中心
 *
 * AnimData 子结构由意图/运动管线直接写入，
 * ZZZAnimSnapshotCapture 直读（单一数据源，无二次拷贝）。
 */
struct FRuntimeData
{
	// ============================================================
	// 帧级意图（由 IntentProcessors 写入，帧末 ResetFrameIntents 清零）
	// ============================================================

	/** 攻击意图（AttackIntentProcessor 写入，ActionArbiter + 状态机 读取） */
	bool bWantsToAttack = false;

	/** 闪避意图（DodgeIntentProcessor 写入，ActionArbiter + 状态机 读取） */
	bool bWantsToDodge = false;

	// ============================================================
	// 移动数据（由 IntentProcessors / MotionDriver 写入）
	// ============================================================

	/** 世界空间移动方向（LocomotionIntentProcessor 写入，MotionDriver + MovementParameterProcessor 读取） */
	FVector DesiredWorldMoveDir = FVector::ZeroVector;

	/** 当前移动速度标量（MotionDriver 写入，LocomotionIntentProcessor 步态解析 读取） */
	float CurrentSpeed = 0.f;

	/** 统一解析的移动步态（LocomotionIntentProcessor 写入，MotionDriver 速度上限 + NTEAnim 读取） */
	EMovementGait ResolvedGait = EMovementGait::None;

	/** 步态速度阈值（cm/s，BaseCharacter::BeginPlay 从 CharacterConfig 同步，MotionDriver + LocomotionIntentProcessor 读取） */
	struct FGaitThresholds
	{
		float Walk = 100.f;
		float Run  = 450.f;
		float Sprint = 600.f;
	} GaitThresholds;

	/** 移动角度（MotionDriver 写入，NTEAnim 读取 → DirectionDecisions） */
	float MoveAngle = 0.f;

	/** 是否在移动（MotionDriver 写入，自身 if 条件判断） */
	bool bIsMoving = false;

	// ============================================================
	// 视角数据（由 ViewRotationProcessor 写入）
	// ============================================================

	/** 控制器旋转（MovementParameterProcessor 读 .Yaw 计算局部分向） */
	FRotator ControlRotation = FRotator::ZeroRotator;

	// ============================================================
	// 物理状态（由 BaseCharacter / MotionDriver 写入）
	// ============================================================

	/** 是否在地面上（InAirState + NTEAnim + ZZZAnim 读取） */
	bool bIsGrounded = true;

	// ============================================================
	// 仲裁标记（由 ArbiterPipeline 每帧写入，MotionDriver + 状态机 读取）
	// ============================================================

	/** 阻止移动（Stunned/Dead 时为 true） */
	bool bBlockMove = false;

	/** 阻止攻击 */
	bool bBlockAttack = false;

	/** 阻止闪避 */
	bool bBlockDodge = false;

	// ============================================================
	// 动作仲裁结果（由 ActionArbiter 写入，状态机 Update 读取）
	// ============================================================

	/** ActionArbiter 批准的动作（Idle = 没有动作被批准） */
	ECharacterStateType ActionGranted = ECharacterStateType::Idle;

	// ============================================================
	// 状态机数据（由 FGYGOStateManager 写入，ActionArbiter + 蓝图 读取）
	// ============================================================

	/** 当前角色状态 */
	ECharacterStateType CurrentState = ECharacterStateType::Idle;

	// ============================================================
	// 动画驱动速度（由 RootMotionParameterProcessor 写入，MotionDriver 读取）
	// ============================================================

	/** 动画驱动速度（cm/s），从根运动位移向量换算而来 */
	float AnimSpeed = 0.f;

	/** 当前帧根骨骼位移向量（cm/帧，本地空间） */
	FVector RootMotionDelta = FVector::ZeroVector;

	/** 是否有有效的根运动数据（RootMotionParameterProcessor 写入，MotionDriver 读取） */
	bool bHasRootMotion = false;

	// ============================================================
	// 动画数据（由管线直接写入，ZZZAnimSnapshotCapture 只读）
	// ============================================================

	FAnimRuntimeData AnimData;

	// ============================================================
	// 帧末清零
	// ============================================================

	void ResetFrameIntents()
	{
		bWantsToAttack = false;
		bWantsToDodge = false;
	}
};

/**
 * @file RuntimeData.h
 * @brief 运行时黑板 - 全局帧级共享数据中枢
 * 
 * 所有子系统通过 RuntimeData 共享数据，不直接互相引用。
 * 每个字段只有一个系统写入，多个系统读取。
 * 帧级意图在帧末由 ResetFrameIntents() 清零。
 */

#pragma once

#include "CoreMinimal.h"
#include "StateMachine/CharacterStateType.h"

/**
 * 运行时黑板
 * 管线中所有系统的数据交换中心
 */
struct FRuntimeData
{
	// ============================================================
	// 输入状态（由 InputPipeline 写入）
	// ============================================================

	/** 移动输入原始值（WASD / 左摇杆） */
	FVector2D MoveInput = FVector2D::ZeroVector;

	/** 视角输入原始值（鼠标 / 右摇杆） */
	FVector2D LookInput = FVector2D::ZeroVector;

	// ============================================================
	// 帧级意图（由 IntentProcessors 写入，帧末清零）
	// ============================================================

	/** 攻击意图（AttackIntentProcessor 写入） */
	bool bWantsToAttack = false;

	/** 闪避意图（DodgeIntentProcessor 写入） */
	bool bWantsToDodge = false;

	/** 冲刺意图（LocomotionIntentProcessor 写入） */
	bool bWantsToSprint = false;

	/** 交互意图 */
	bool bWantsToInteract = false;

	// ============================================================
	// 移动数据（由 IntentProcessors / MotionDriver 写入）
	// ============================================================

	/** 世界空间移动方向（LocomotionIntentProcessor 写入） */
	FVector DesiredWorldMoveDir = FVector::ZeroVector;

	/** 当前移动速度标量（MotionDriver 写入） */
	float CurrentSpeed = 0.f;

	/** 移动角度，相对于角色朝向（MotionDriver 写入） */
	float MoveAngle = 0.f;

	/** 是否在移动（MotionDriver 写入） */
	bool bIsMoving = false;

	// ============================================================
	// 视角数据（由 ViewRotationProcessor 写入）
	// ============================================================

	/** 视角水平旋转角度 */
	float ViewYaw = 0.f;

	/** 视角垂直旋转角度 */
	float ViewPitch = 0.f;

	/** 控制器旋转（由 ViewYaw/Pitch 计算得出） */
	FRotator ControlRotation = FRotator::ZeroRotator;

	// ============================================================
	// 物理状态（由 BaseCharacter / MotionDriver 写入）
	// ============================================================

	/** 是否在地面上 */
	bool bIsGrounded = true;

	/** 垂直速度 */
	float VerticalVelocity = 0.f;

	/** 刚刚落地（落地那一帧为 true，帧末清零） */
	bool bJustLanded = false;

	// ============================================================
	// 仲裁标记（由 ArbiterPipeline 每帧写入）
	// ============================================================

	/** 阻止移动（Stunned/Dead 时为 true） */
	bool bBlockMove = false;

	/** 阻止攻击 */
	bool bBlockAttack = false;

	/** 阻止闪避 */
	bool bBlockDodge = false;

	/** 阻止所有输入处理 */
	bool bBlockInput = false;

	// ============================================================
	// 动作仲裁结果（由 ActionArbiter 写入）
	// ============================================================

	/** ActionArbiter 批准的动作（Idle = 没有动作被批准） */
	ECharacterStateType ActionGranted = ECharacterStateType::Idle;

	// ============================================================
	// 状态机数据（由 FCharacterStateMachine::PerformTransition 写入）
	// ============================================================

	/**
	 * 当前角色状态
	 * 由 FCharacterStateMachine 在状态切换时写入。
	 * 动画蓝图、UI 等只读此字段获取当前状态。
	 */
	ECharacterStateType CurrentState = ECharacterStateType::Idle;

	// ============================================================
	// 动画驱动速度（由 RootMotionParameterProcessor 写入）
	// ============================================================

	/**
	 * 动画驱动速度（cm/s），从根运动位移向量换算而来。
	 * 公式：AnimSpeed = |RootMotionDelta| / DeltaTime
	 *
	 * 数据来源：GGYGOAnimInstance::ExtractRootMotionDelta 从当前
	 * AnimSequence 内置根骨骼轨道提取每帧位移 → RootMotionDelta
	 * → RootMotionParameterProcessor 换算为速度标量。
	 */
	float AnimSpeed = 0.f;

	/** 当前动画是否有有效的根运动数据（RootMotionParameterProcessor 检测到非零 RM 时设为 true） */
	bool bBip001Found = false;

	// ============================================================
	// Root Motion（由 GGYGOAnimInstance 提取，RootMotionParameterProcessor 同步）
	// ============================================================

	/**
	 * 当前帧根骨骼位移向量（cm/帧，本地空间）。
	 * 由 GGYGOAnimInstance::ExtractRootMotionDelta 从 AnimSequence 内置根骨骼轨道提取。
	 * RootMotionParameterProcessor 每帧同步此值到 RuntimeData。
	 */
	FVector RootMotionDelta = FVector::ZeroVector;

	/** 是否有有效的根运动数据（RootMotionParameterProcessor 写入） */
	bool bHasRootMotion = false;

	// ============================================================
	// AnimNotify 通知标记（由 GGYGOAnimInstance::OnRunStartFinished 等写入，状态机读取）
	// ============================================================

	/** RunStart 动画播放完成标记（AnimNotify 写入，RunStartState::Update 读取后转换到 RunLoop） */
	bool bRunStartFinished = false;

	/** RunEnd 动画播放完成标记（AnimNotify 写入，RunEndState::Update 读取后转换到 Idle） */
	bool bRunEndFinished = false;

	/** RunStart 动画最短播放时间（秒），从 MovementConfig 同步 */
	float RunStartMinDuration = 0.25f;

	/** RunEnd 动画最短播放时间（秒），从 MovementConfig 同步 */
	float RunEndMinDuration = 0.25f;

	// ============================================================
	// 动画参数（由 MovementParameterProcessor 写入）
	// ============================================================

	/** Blend SpaceX 轴参数 */
	float AnimBlendX = 0.f;

	/** Blend SpaceY 轴参数 */
	float AnimBlendY = 0.f;

	/** 动画播放倍率（由 MoveDriverComponent 计算） */
	float PlayRate = 1.0f;

	// ============================================================
	// 帧末清零
	// ============================================================

	/**
	 * 清零所有帧级意图标记
	 * 在 BaseCharacter::Tick 末尾调用
	 * 下一帧由 IntentProcessors 重新计算
	 */
	void ResetFrameIntents()
	{
		bWantsToAttack = false;
		bWantsToDodge = false;
		bWantsToSprint = false;
		bWantsToInteract = false;
		// TODO: ActionGranted = ECharacterStateType::Idle;
		bJustLanded = false;
	}
};

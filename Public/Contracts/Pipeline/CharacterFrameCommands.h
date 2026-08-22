/**
 * @file CharacterFrameCommands.h
 * @brief 角色控制管线本帧可执行提交命令
 *
 * 这里只放当前源码中已经存在真实提交实现的命令。GameplayAbility、攻击、
 * 闪避和额外状态命令在对应运行时链路接通前不在此处伪造。
 */
#pragma once

#include "CoreMinimal.h"
#include "StateMachine/CharacterStateType.h"

/** 由 Pipeline 提交给 MotionDriver 的本帧移动命令。 */
struct FCharacterMovementCommand
{
	/** 是否执行本帧移动提交；Pipeline 每次 Decision 后显式设置。 */
	bool bShouldCommit = false;

	/** 本帧是否禁止移动。 */
	bool bBlockMove = false;

	/** 普通移动和 TurnBack Released 使用的世界移动方向。 */
	FVector DesiredWorldMoveDir = FVector::ZeroVector;

	/** TurnBack 方向解析所需的状态和 Frozen 入口方向。 */
	ETurnBackPhase TurnBackPhase = ETurnBackPhase::None;
	FVector TurnBackEntryDirection = FVector::ZeroVector;

	/** 本帧 RootMotion 曲线速度和位置差分路径选择。 */
	float AnimCurveSpeed = 0.f;
	float AnimCurveYaw = 0.f;
	/** RM_PosX/RM_PosY 的局部位移差分；Released 首帧作为方向候选，与目标输入同向时直接使用，相反时取 180°反向。 */
	FVector RootMotionDelta = FVector::ZeroVector;
	bool bHasRootMotion = false;

	/** 恢复为空命令，避免未重新构造时重复提交上一帧数据。 */
	void Reset()
	{
		bShouldCommit = false;
		bBlockMove = false;
		DesiredWorldMoveDir = FVector::ZeroVector;
		TurnBackPhase = ETurnBackPhase::None;
		TurnBackEntryDirection = FVector::ZeroVector;
		AnimCurveSpeed = 0.f;
		AnimCurveYaw = 0.f;
		RootMotionDelta = FVector::ZeroVector;
		bHasRootMotion = false;
	}
};

/** 由 Pipeline 提交给已有 UZZZAnimInstance 的动画发布命令。 */
struct FCharacterAnimationPublishCommand
{
	/** 是否在 Animation Publish 阶段调用 PipelineDrive。 */
	bool bShouldPublish = false;

	void Reset()
	{
		bShouldPublish = false;
	}
};

/** 本帧真正可执行的外部提交命令集合。 */
struct FCharacterFrameCommandBuffer
{
	FCharacterMovementCommand Movement;
	FCharacterAnimationPublishCommand Animation;

	void Reset()
	{
		Movement.Reset();
		Animation.Reset();
	}
};

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

	/** 本帧是否存在有效移动意图；普通零输入帧由 MotionDriver 用它阻止曲线继续驱动移动。 */
	bool bShouldMove = false;

	/** 本帧是否禁止移动。 */
	bool bBlockMove = false;

	/** 摄像机相对输入转换得到的世界移动方向；仅在没有有效根轨迹曲线时作为回退。 */
	FVector DesiredWorldMoveDir = FVector::ZeroVector;

	/** TurnBack 逻辑相位；由 Pipeline 从 RuntimeData 复制给 MotionDriver 和动画投影。 */
	ETurnBackPhase TurnBackPhase = ETurnBackPhase::None;

	/** TurnBack 是否已经进入第二段；由逻辑时间轴决定 d1 的选择。 */
	bool bTurnBackSecondSegment = false;

	/** 本帧 RootMotion 曲线速度和位置差分路径选择。 */
	float AnimCurveSpeed = 0.f;

	/** RM_Yaw 累计曲线相邻采样值的差分（度）；仅在 TurnBack d0 由 MotionDriver 应用到 Actor yaw。 */
	float AnimCurveYawDelta = 0.f;

	/** 固定动画起始根轨迹坐标中的速度（cm/s），与 RM_VelocityDirX/Y 使用同一 X=前、Y=右坐标系。 */
	FVector AnimCurveVelocity = FVector::ZeroVector;

	/** 根轨迹最终有效方向；RM_VelocityDirX/Y authored 有效时优先，否则使用 RM_PosX/RM_PosY 差分。 */
	FVector AnimCurveVelocityDirection = FVector::ZeroVector;

	/** 当前方向是否来自 authored RM_VelocityDirX/Y，而不是 RM_PosX/RM_PosY 差分回退。 */
	bool bHasAuthoredVelocityDirection = false;

	/** 当前帧是否存在有效 RootMotion 曲线源。 */
	bool bHasRootMotionCurveSource = false;

	/** RM_PosX/RM_PosY 的固定动画起始根轨迹位移差分；有效时参与曲线方向和位移路径。 */
	FVector RootMotionDelta = FVector::ZeroVector;
	bool bHasRootMotion = false;

	/** 恢复为空命令，避免未重新构造时重复提交上一帧数据。 */
	void Reset()
	{
		bShouldCommit = false;
		bShouldMove = false;
		bBlockMove = false;
		DesiredWorldMoveDir = FVector::ZeroVector;
		TurnBackPhase = ETurnBackPhase::None;
		bTurnBackSecondSegment = false;
		AnimCurveSpeed = 0.f;
		AnimCurveYawDelta = 0.f;
		AnimCurveVelocity = FVector::ZeroVector;
		AnimCurveVelocityDirection = FVector::ZeroVector;
		bHasAuthoredVelocityDirection = false;
		bHasRootMotionCurveSource = false;
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

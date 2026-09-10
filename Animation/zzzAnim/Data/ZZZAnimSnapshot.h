/**
 * @file ZZZAnimSnapshot.h
 * @brief ZZZ 动画快照结构体
 *
 * 每帧从移动层抓取一份不变的值，供 worker 线程与决策函数读取。
 *
 * ## 为什么要快照
 * AnimBP 的过渡条件可能在 worker 线程上求值，而移动层的状态在游戏线程被改写，
 * 直接读会有数据竞争。快照把读取时机收敛到游戏线程的一个点上，
 * 之后整帧内所有判定看到的都是同一份值 —— 这也顺带保证了同一帧里
 * 多个过渡条件不会基于不同时刻的状态做出互相矛盾的决定。
 *
 * ## 数据来源
 * 全部来自 `UGGYGOCharacterMovementComponent`，由 `FZZZAnimSnapshotCapture` 抓取。
 *
 * 其中 TurnBack 三项与 AnimCurve 三项**恒为默认值**：它们依赖动画曲线采样，
 * 而曲线采样尚未接入 CMC。后果是转身表现不会触发，走跑正常。
 */
#pragma once

#include "Character/Data/GGYGOMovementTypes.h"
#include "CoreMinimal.h"

struct FZZZAnimSnapshot
{
	// ---- Locomotion ----

	/** 当前步态（None/Walk/Run）。步态判定与 GaitBlendY 目标值的唯一来源。 */
	EGGYGOGait Gait = EGGYGOGait::None;

	/**
	 * 本帧是否有移动意图。
	 *
	 * 同时承担"角色是否处于移动状态"的判定 —— 两者永远同值，
	 * 因为移动状态本身就是有无移动输入的投影，不需要两个字段。
	 */
	bool bShouldMove = false;

	/** 相对 Actor 当前水平朝向的移动方向 X（右）和 Y（前）。BlendSpace 的两个轴输入。 */
	float AnimBlendX = 0.f;
	float AnimBlendY = 0.f;

	/**
	 * 动画曲线分量速度（cm/s，X=左右、Y=前后）。
	 * 曲线采样尚未接入，恒为零向量。
	 */
	FVector AnimCurveVelocity = FVector::ZeroVector;

	/** 上者的归一化方向（原始曲线分量系）。曲线采样尚未接入，恒为零向量。 */
	FVector AnimCurveVelocityDirection = FVector::ZeroVector;

	/** 动画曲线速度方向角（度）。曲线采样尚未接入，恒为 0。 */
	float AnimCurveVelocityAngle = 0.f;

	/** 移动输入与角色当前水平前向的点积；1 为同向，-1 为完全反向。仅诊断用途。 */
	float InputForwardDot = 1.f;

	/**
	 * 转身相位。WalkRun/TurnBack 过渡判定的唯一依据。
	 *
	 * 相位机尚未在 CMC 内实现，恒为 `None`，因此转身状态不会被进入。
	 */
	EGGYGOTurnBackPhase TurnBackPhase = EGGYGOTurnBackPhase::None;

	/** 已消费 CanYaw Notify，输入接管许可。相位机尚未实现，恒为 false。 */
	bool bCanYaw = false;

	/** 转身是否已进入第二段。相位机尚未实现，恒为 false。 */
	bool bTurnBackSecondSegment = false;

	/** 当前水平速度标量（cm/s）。 */
	float VelocityLength = 0.f;

	/** 角色实际水平速度的世界空间单位方向。 */
	FVector ActualVelocityDirection = FVector::ZeroVector;

	/** 实际速度相对 Actor 的 BlendSpace 分量：X=右，Y=前。 */
	float ActualVelocityBlendX = 0.f;
	float ActualVelocityBlendY = 0.f;

	/** 实际速度相对 Actor 的方向角（度）：0=前，+90=右。 */
	float ActualVelocityAngle = 0.f;

	// ---- 通用 ----

	/** 是否站在地面上。 */
	bool bGrounded = true;

	/** 是否被 `Restriction.CantMove` 禁止移动。 */
	bool bBlockMove = false;
};

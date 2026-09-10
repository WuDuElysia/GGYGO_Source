/**
 * @file ZZZAnimSnapshot.h
 * @brief ZZZ 动画快照结构体
 *
 * 每帧从移动层抓取一份不变的值，供 worker 线程与决策函数读取。
 *
 * ## 为什么要快照
 * AnimBP 的过渡条件可能在 worker 线程上求值，而移动层的状态在游戏线程被改写。
 * 直接读会有数据竞争。快照把"读取时机"收敛到游戏线程的一个点上，
 * 之后整帧内所有判定看到的都是同一份值 —— 这也顺带保证了同一帧里
 * 多个过渡条件不会基于不同时刻的状态做出互相矛盾的决定。
 *
 * ## 数据来源（阶段 5 起）
 * 全部来自 `UGGYGOCharacterMovementComponent`。此前来自 `FRuntimeData`，
 * 那条链路随移动 Pipeline 一并退役。
 *
 * 其中 TurnBack 三项与 AnimCurve 三项当前**恒为默认值** ——
 * 它们依赖动画曲线采样，那是阶段 6 的内容。期间转身表现不触发，走跑正常。
 */
#pragma once

#include "Character/Data/GGYGOMovementTypes.h"
#include "CoreMinimal.h"

struct FZZZAnimSnapshot
{
	// ---- Locomotion ----

	/** 当前步态（None/Walk/Run）← `CMC::GetResolvedGait()`。步态判定与 GaitBlendY 目标值的唯一来源。 */
	EGGYGOGait Gait = EGGYGOGait::None;

	/**
	 * 本帧是否有移动意图 ← `CMC::HasMoveInput()`。
	 *
	 * 这个字段同时承担了旧快照里 `CurrentState == Moving` 的职责 ——
	 * 那个逻辑状态本身就是"有无移动输入"的投影（`FIdleState` 见到方向就转 Moving，
	 * `FMovingState` 见到方向归零就转回），两者永远同值，属于冗余状态。
	 */
	bool bShouldMove = false;

	/** 相对 Actor 当前水平朝向的平滑移动方向 X（右）和 Y（前）← `CMC::GetLocalVelocityBlend()`。 */
	float AnimBlendX = 0.f;
	float AnimBlendY = 0.f;

	/**
	 * RM_PosX/RM_PosY 差分得到的原始曲线分量速度（cm/s，X=左右、Y=前后）。
	 * **阶段 6 才有生产者**，当前恒为零向量。
	 */
	FVector AnimCurveVelocity = FVector::ZeroVector;

	/** 上者的归一化方向（原始曲线分量系）。**阶段 6 才有生产者**。 */
	FVector AnimCurveVelocityDirection = FVector::ZeroVector;

	/** 动画曲线速度方向角（度）。**阶段 6 才有生产者**。 */
	float AnimCurveVelocityAngle = 0.f;

	/** 移动输入与角色当前水平前向的点积；1 为同向，-1 为完全反向。仅诊断用途。 */
	float InputForwardDot = 1.f;

	/**
	 * TurnBack 相位。WalkRun/TurnBack 过渡判定的唯一依据。
	 * **阶段 6 才有生产者**，当前恒为 `None`，转身状态不会被进入。
	 */
	EGGYGOTurnBackPhase TurnBackPhase = EGGYGOTurnBackPhase::None;

	/** 已消费 CanYaw Notify，输入接管许可。**阶段 6 才有生产者**。 */
	bool bCanYaw = false;

	/** TurnBack 是否已进入第二段。**阶段 6 才有生产者**。 */
	bool bTurnBackSecondSegment = false;

	/** 当前水平速度标量（cm/s）← `CMC::GetHorizontalSpeed()`。 */
	float VelocityLength = 0.f;

	/** 角色实际水平速度的世界空间单位方向 ← `CMC::GetHorizontalVelocityDirection()`。 */
	FVector ActualVelocityDirection = FVector::ZeroVector;

	/** 实际速度相对 Actor 的 BlendSpace 分量：X=右，Y=前 ← `CMC::GetLocalVelocityBlend()`。 */
	float ActualVelocityBlendX = 0.f;
	float ActualVelocityBlendY = 0.f;

	/** 实际速度相对 Actor 的方向角（度）：0=前，+90=右 ← `CMC::GetLocalVelocityAngle()`。 */
	float ActualVelocityAngle = 0.f;

	// ---- 通用 ----

	/**
	 * 是否站在地面上 ← `CMC::IsMovingOnGround()`。
	 *
	 * 旧实现里这个值来自 `RuntimeData.Movement.bIsGrounded`，而那个字段
	 * **没有任何运行时写入方**，恒为 true。改从 CMC 直接取顺带修掉了这个缺陷。
	 */
	bool bGrounded = true;

	/** 是否被禁止移动 ← `CMC::IsMovementBlockedByTag()`。旧实现此处硬编码 false。 */
	bool bBlockMove = false;
};

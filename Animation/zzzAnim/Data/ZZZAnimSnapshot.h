/**
 * @file ZZZAnimSnapshot.h
 * @brief ZZZ 动画快照结构体
 *
 * 每帧从项目通用 AnimationStateFrame 适配一份不变的值，供旧表现记忆与调试字段读取。
 *
 * ## 为什么要快照
 * AnimBP 的过渡条件可能在 worker 线程上求值，而移动层的状态在游戏线程被改写，
 * 直接读会有数据竞争。快照把读取时机收敛到游戏线程的一个点上，
 * 之后整帧内兼容表现逻辑看到的都是同一份值。
 *
 * ## 数据来源
 * 稳定语义来自 `FGGYGOAnimationStateFrame`，旧调试字段来自
 * `FGGYGOAnimationDebugFrame`，由 `FZZZAnimSnapshotCapture` 做纯数据映射。
 * 本结构只服务迁移期兼容，不再直接读取 Actor、CMC 或 ASC。
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

	/**
	 * 相对 Actor 当前水平朝向的移动方向 X（右）和 Y（前）。
	 *
	 * 目前无人消费：走跑混合是一维的、由步态混合值驱动，方向靠 Actor 转向解决。
	 * 等有了侧向/后退的移动循环动画换成二维 BlendSpace 后才会接上。
	 */
	float AnimBlendX = 0.f;
	float AnimBlendY = 0.f;

	/**
	 * 动画曲线分量速度（cm/s）。
	 *
	 * 轴序是 UE 局部空间（X 前、Y 右），但基准是**动画段起点的朝向**，
	 * 不是角色当前朝向。转身时两者会分离：角色已经转过 180 度，
	 * 而这个向量仍以进入转身那一刻的朝向为基准。
	 */
	FVector AnimCurveVelocity = FVector::ZeroVector;

	/** 上者的归一化方向。同样以动画段起点朝向为基准。 */
	FVector AnimCurveVelocityDirection = FVector::ZeroVector;

	/** 动画曲线速度方向角（度）：0 为段起点正前方，+90 为其右侧。 */
	float AnimCurveVelocityAngle = 0.f;

	/** 移动输入与角色当前水平前向的点积；1 为同向，-1 为完全反向。仅诊断用途。 */
	float InputForwardDot = 1.f;

	/** 转身相位。WalkRun/TurnBack 过渡判定的唯一依据。 */
	EGGYGOTurnBackPhase TurnBackPhase = EGGYGOTurnBackPhase::None;

	/** 转身是否已进入交还输入的 `RunOut` 段。 */
	bool bTurnBackRunOut = false;

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

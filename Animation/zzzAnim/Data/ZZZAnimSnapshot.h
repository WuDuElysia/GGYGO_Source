/**
 * @file ZZZAnimSnapshot.h
 * @brief ZZZ 动画快照结构体
 *
 * 游戏线程从 FZZZAnimRuntimeModel + Owner 抓取，
 * worker 线程与决策函数读取此快照；一次性过渡触发开关由对应决策消费。
 *
 * 需要捕获的字段在此结构体中声明，
 * 捕获实现在 Capture/ZZZAnimSnapshotCapture.cpp 中。
 */
#pragma once

#include "CoreMinimal.h"
#include "StateMachine/CharacterStateType.h"  // EMovementGait, ETurnBackPhase

struct FZZZAnimSnapshot
{
	// ---- Locomotion ----

	/** 当前步态（None/Walk/Run）← ZZZAnimRuntimeModel.Gait；动画层步态判定与 GaitBlendY 目标值的唯一来源 */
	EMovementGait Gait = EMovementGait::None;

	/** 是否有移动输入/意图 ← AnimRuntimeData.bShouldMove */
	bool bShouldMove = false;

	/** 相对 Actor 当前水平朝向的平滑移动方向 X（右）和 Y（前）。 */
	float AnimBlendX = 0.f;
	float AnimBlendY = 0.f;

	/** RM_PosX/RM_PosY 差分得到的原始曲线分量速度（cm/s，X=左右、Y=前后）。 */
	FVector AnimCurveVelocity = FVector::ZeroVector;

	/** RM_PosX/RM_PosY 差分速度的归一化原始曲线分量方向；MotionDriver 转换为 UE 局部 X=前、Y=右。 */
	FVector AnimCurveVelocityDirection = FVector::ZeroVector;

	/** 动画曲线速度方向角（度），等同于 RootMotionRuntimeModel.AnimCurveAngle。 */
	float AnimCurveVelocityAngle = 0.f;

	/** 摄像机修正后的移动输入与角色当前水平前向的点积；1 为同向，-1 为完全反向。仅诊断用途。 */
	float InputForwardDot = 1.f;

	/** TurnBack 相位 ← RuntimeData.Movement.TurnBack.Phase；WalkRun/TurnBack 过渡判定的唯一依据。 */
	ETurnBackPhase TurnBackPhase = ETurnBackPhase::None;

	/** 逻辑参数阶段已消费 CanYaw Notify；动画层只读该输入接管许可。 */
	bool bCanYaw = false;

	/** TurnBack 是否已经进入逻辑第二段；动画层只读该标记，不从动画曲线反推分段。 */
	bool bTurnBackSecondSegment = false;

	/** 当前角色状态 ← ZZZAnimRuntimeModel.CurrentState */
	ECharacterStateType CurrentState = ECharacterStateType::Idle;

	/** 当前速度标量（cm/s）← AnimRuntimeData.VelocityLength */
	float VelocityLength = 0.f;

	/** 角色实际水平速度的世界空间单位方向 ← ZZZAnimRuntimeModel.ActualVelocityDirection。 */
	FVector ActualVelocityDirection = FVector::ZeroVector;

	/** 角色实际速度相对 Actor 的 BlendSpace 分量：X=右，Y=前。 */
	float ActualVelocityBlendX = 0.f;
	float ActualVelocityBlendY = 0.f;

	/** 角色实际速度相对 Actor 的方向角（度）。 */
	float ActualVelocityAngle = 0.f;

	// ---- 通用 ----

	/** 是否在地面 ← ABaseCharacter::IsGrounded() */
	bool bGrounded = true;

	/** 是否被仲裁阻止移动 ← 暂无来源，恒 false */
	bool bBlockMove = false;
};

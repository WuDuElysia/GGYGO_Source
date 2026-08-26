/**
 * @file ZZZAnimRuntimeModel.h
 * @brief 新版 ZZZ 动画层的游戏线程运行时投影
 *
 * 逻辑管线每帧写入 → ZZZAnimSnapshotCapture 只读抓取 → FZZZAnimSnapshot
 * → ZZZAnimContext / AnimBP。
 * 该 model 只描述 ZZZ Locomotion 所需的投影，不拥有逻辑决策。
 */
#pragma once

#include "CoreMinimal.h"
#include "StateMachine/CharacterStateType.h"

/**
 * 新版 ZZZ 动画层运行时投影（纯 C++ 结构体，无反射）。
 *
 * 逻辑层字段是 canonical 数据，本结构只作为同帧动画输入的显式投影。
 */
struct FZZZAnimRuntimeModel
{
	/** 步态（None / Walk / Run），FGaitAuthorityProcessor 同帧写入。 */
	EMovementGait Gait = EMovementGait::None;

	/** 有移动输入 / 应移动，LocomotionIntentProcessor 同帧写入。 */
	bool bShouldMove = false;

	/** 相对 Actor 当前水平朝向的平滑移动方向 X（右）和 Y（前）。 */
	float AnimBlendX = 0.f;
	float AnimBlendY = 0.f;

	/** 当前角色状态，StateManager 与逻辑状态同源写入。 */
	ECharacterStateType CurrentState = ECharacterStateType::Idle;

	/** 当前速度标量（cm/s），MotionDriver 与逻辑速度同帧写入。 */
	float VelocityLength = 0.f;

	/** 角色实际水平速度的世界空间单位方向，MotionDriver 在移动提交后写入。 */
	FVector ActualVelocityDirection = FVector::ZeroVector;

	/** 角色实际速度相对 Actor 的 BlendSpace 分量：X=右，Y=前。 */
	float ActualVelocityBlendX = 0.f;
	float ActualVelocityBlendY = 0.f;

	/** 角色实际速度相对 Actor 的方向角（度）：0=前，+90=右。 */
	float ActualVelocityAngle = 0.f;

	/** 2D 速度（cm/s），保留现有动画数据契约。 */
	float Velocity2DLength = 0.f;

	/** 输入方向角度（度），保留现有动画数据契约。 */
	float LastInputDirectionAngle = 0.f;
};

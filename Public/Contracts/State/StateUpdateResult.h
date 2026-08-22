/**
 * @file StateUpdateResult.h
 * @brief StateManager 向角色控制管线输出的状态更新结果
 *
 * 状态规则、关系矩阵、Enter/Exit 和状态相关副作用仍由 StateManager 所有；
 * 本文件只描述一次状态更新完成后的结果，供 Pipeline 形成帧计划和发布外部提交。
 */
#pragma once

#include "CoreMinimal.h"
#include "StateMachine/CharacterStateType.h"

/** StateManager 在一次状态操作中产生的结果类型。 */
enum class EStateTransitionEventType : uint8
{
	Activated,
	Released,
	Forced,
	Rejected,
};

/** 一次状态请求或状态生命周期操作的结果。 */
struct FStateTransitionEvent
{
	/** 本次操作的类型。 */
	EStateTransitionEventType Type = EStateTransitionEventType::Rejected;

	/** 请求的目标状态；拒绝事件也保留该值用于诊断。 */
	ECharacterStateType RequestedState = ECharacterStateType::None;

	/** 操作前后的 Locomotion PrimaryState。 */
	ECharacterStateType PreviousPrimaryState = ECharacterStateType::Idle;
	ECharacterStateType CurrentPrimaryState = ECharacterStateType::Idle;

	/** 操作是否被 StateManager 接受并完成。 */
	bool bAccepted = false;

	/** PrimaryState 是否因本次操作发生变化。 */
	bool bPrimaryStateChanged = false;

	/** 本次激活前被 Interrupted 的状态类型。 */
	TArray<ECharacterStateType> InterruptedStates;

	/** 恢复为空事件，供帧级结果重用。 */
	void Reset()
	{
		Type = EStateTransitionEventType::Rejected;
		RequestedState = ECharacterStateType::None;
		PreviousPrimaryState = ECharacterStateType::Idle;
		CurrentPrimaryState = ECharacterStateType::Idle;
		bAccepted = false;
		bPrimaryStateChanged = false;
		InterruptedStates.Reset();
	}
};

/** StateManager 完成一次 Update 后的聚合结果。 */
struct FStateUpdateResult
{
	/** 本次 Update 开始时的 Locomotion PrimaryState。 */
	ECharacterStateType PreviousPrimaryState = ECharacterStateType::Idle;

	/** 本次 Update 完成后的 Locomotion PrimaryState。 */
	ECharacterStateType CurrentPrimaryState = ECharacterStateType::Idle;

	/** 本次 Update 是否改变了 PrimaryState。 */
	bool bStateChanged = false;

	/** 本次 Update 内产生的状态操作结果，按实际发生顺序排列。 */
	TArray<FStateTransitionEvent> Events;

	/** 清空结果并设置本次更新的起始状态。 */
	void Reset(ECharacterStateType InPreviousPrimaryState = ECharacterStateType::Idle)
	{
		PreviousPrimaryState = InPreviousPrimaryState;
		CurrentPrimaryState = InPreviousPrimaryState;
		bStateChanged = false;
		Events.Reset();
	}
};

/**
 * @file RunEndState.cpp
 * @brief 跑步停止状态实现
 *
 * 停止动画播放期间：
 * - ActionGranted 优先（仲裁批准的动作可打断停止动画）
 * - 又开始移动 → 打断 End，回到 RunStart
 * - 动画播完 → 回到 Idle
 */
#include "StateMachine/State/RunEndState.h"
#include "StateMachine/CharacterStateMachine.h"
#include "Data/RuntimeData.h"

void FRunEndState::Enter(FRuntimeData& RuntimeData)
{
	RuntimeData.bRunEndFinished = false;
}

void FRunEndState::Update(float DeltaTime, FRuntimeData& RuntimeData, FCharacterStateMachine& SM)
{
	// ============================================================
	// 优先级 0：ActionArbiter 批准的动作（可打断停止动画）
	// ============================================================

	if (RuntimeData.ActionGranted != ECharacterStateType::Idle)
	{
		SM.TryTransitionTo(RuntimeData.ActionGranted, RuntimeData);
		return;
	}

	// ============================================================
	// 优先级 1：又开始移动 → 打断停止动画，回到启动
	// ============================================================

	if (!RuntimeData.DesiredWorldMoveDir.IsNearlyZero())
	{
		SM.TryTransitionTo(ECharacterStateType::RunStart, RuntimeData);
		return;
	}

	// ============================================================
	// 优先级 2：动画播完 → 回到 Idle
	// ============================================================

	if (RuntimeData.bRunEndFinished)
	{
		SM.TryTransitionTo(ECharacterStateType::Idle, RuntimeData);
		return;
	}
}

void FRunEndState::Exit(FRuntimeData& RuntimeData)
{
	RuntimeData.bRunEndFinished = false;
}

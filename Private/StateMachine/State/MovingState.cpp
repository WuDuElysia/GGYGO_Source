/**
 * @file MovingState.cpp
 * @brief 移动状态实现
 *
 * 移动状态下按优先级检查：ActionGranted > 停止移动。
 * GAS 接入后逐步添加攻击/闪避优先级。
 */
#include "StateMachine/State/MovingState.h"
#include "StateMachine/GGYGOStateManager.h"
#include "Data/Logic/RuntimeData.h"

void FMovingState::Update(float DeltaTime, FRuntimeData& RuntimeData, FGYGOStateManager& SM)
{
	// Priority 0: ActionArbiter 批准的动作（GAS 接入后启用）
	if (RuntimeData.ActionGranted != ECharacterStateType::Idle &&
		RuntimeData.ActionGranted != ECharacterStateType::Moving)
	{
		SM.RequestState(RuntimeData.ActionGranted);
		return;
	}

	// Priority 1: 松手 → Idle
	if (RuntimeData.DesiredWorldMoveDir.IsNearlyZero() && !RuntimeData.bBlockMove)
	{
		SM.RequestState(ECharacterStateType::Idle);
		return;
	}
}

/**
 * @file IdleState.cpp
 * @brief 待机状态实现
 *
 * 待机状态按优先级检查：ActionGranted > 移动。
 * GAS 接入后逐步添加攻击/闪避优先级。
 */
#include "StateMachine/State/IdleState.h"
#include "StateMachine/GGYGOStateManager.h"
#include "Data/Runtime/RuntimeData.h"

void FIdleState::Enter(FRuntimeData& RuntimeData)
{
}

void FIdleState::Update(float DeltaTime, FRuntimeData& RuntimeData, FGYGOStateManager& SM)
{
	// Priority 0: ActionArbiter 批准的动作（GAS 接入后启用）
	if (RuntimeData.Arbiter.ActionGranted != ECharacterStateType::Idle)
	{
		SM.RequestState(RuntimeData.Arbiter.ActionGranted);
		return;
	}

	// Priority 1: 移动 → Moving（速度上限由 MotionDriver 管理）
	if (!RuntimeData.Intent.DesiredWorldMoveDir.IsNearlyZero() && !RuntimeData.Arbiter.bBlockMove)
	{
		SM.RequestState(ECharacterStateType::Moving);
		return;
	}
}

void FIdleState::Exit(FRuntimeData& RuntimeData)
{
}

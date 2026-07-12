/**
 * @file InAirState.cpp
 * @brief 空中状态实现
 */
#include "StateMachine/State/InAirState.h"
#include "StateMachine/GGYGOStateManager.h" // ★ 阶段6
#include "Data/RuntimeData.h"

void FInAirState::Update(float DeltaTime, FRuntimeData& RuntimeData, FGYGOStateManager& SM)
{
	if (RuntimeData.bIsGrounded)
	{
		if (RuntimeData.DesiredWorldMoveDir.IsNearlyZero())
			SM.RequestState(ECharacterStateType::Idle);
		else
			SM.RequestState(ECharacterStateType::Moving);
	}
}

/**
 * @file InAirState.cpp
 * @brief 空中状态实现
 */
#include "StateMachine/State/InAirState.h"
#include "StateMachine/CharacterStateMachine.h"
#include "Data/RuntimeData.h"

void FInAirState::Update(float DeltaTime, FRuntimeData& RuntimeData, FCharacterStateMachine& SM)
{
	if (RuntimeData.bIsGrounded)
	{
		if (RuntimeData.DesiredWorldMoveDir.IsNearlyZero())
		{
			SM.TryTransitionTo(ECharacterStateType::Idle, RuntimeData);
		}
		else
		{
			SM.TryTransitionTo(ECharacterStateType::RunStart, RuntimeData);
		}
	}
}

#include "StateMachine/State/MovingState.h"
#include "StateMachine/GGYGOStateManager.h"
#include "Data/RuntimeData.h"

void FMovingState::Update(float DeltaTime, FRuntimeData& RuntimeData, FGYGOStateManager& SM)
{
	// Priority 0: ActionArbiter 批准的动作
	if (RuntimeData.ActionGranted != ECharacterStateType::Idle &&
		RuntimeData.ActionGranted != ECharacterStateType::Moving)
	{
		SM.RequestState(RuntimeData.ActionGranted);
		return;
	}

	// Priority 1: 闪避
	if (RuntimeData.bWantsToDodge && !RuntimeData.bBlockDodge)
	{
		SM.RequestState(ECharacterStateType::Dodging);
		return;
	}

	// Priority 2: 攻击
	if (RuntimeData.bWantsToAttack && !RuntimeData.bBlockAttack)
	{
		SM.RequestState(ECharacterStateType::Attacking);
		return;
	}

	// Priority 3: 松手 → Idle
	if (RuntimeData.DesiredWorldMoveDir.IsNearlyZero() && !RuntimeData.bBlockMove)
	{
		SM.RequestState(ECharacterStateType::Idle);
		return;
	}
}

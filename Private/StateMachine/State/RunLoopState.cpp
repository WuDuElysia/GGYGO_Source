/**
 * @file RunLoopState.cpp
 * @brief 跑步循环状态实现
 *
 * 循环跑步期间的检查顺序：
 * - ActionGranted 优先（仲裁批准的动作可打断循环跑）
 * - 松手 → RunEnd
 * - 持续输入 → 继续循环
 */
#include "StateMachine/State/RunLoopState.h"
#include "StateMachine/CharacterStateMachine.h"
#include "Data/RuntimeData.h"

void FRunLoopState::Update(float DeltaTime, FRuntimeData& RuntimeData, FCharacterStateMachine& SM)
{
	// ============================================================
	// 优先级 0：ActionArbiter 批准的动作（可打断循环跑）
	// ============================================================

	if (RuntimeData.ActionGranted != ECharacterStateType::Idle)
	{
		SM.TryTransitionTo(RuntimeData.ActionGranted, RuntimeData);
		return;
	}

	// ============================================================
	// 优先级 1：闪避
	// ============================================================

	if (RuntimeData.bWantsToDodge && !RuntimeData.bBlockDodge)
	{
		SM.TryTransitionTo(ECharacterStateType::Dodging, RuntimeData);
		return;
	}

	// ============================================================
	// 优先级 2：攻击（备用路径）
	// ============================================================

	if (RuntimeData.bWantsToAttack && !RuntimeData.bBlockAttack)
	{
		SM.TryTransitionTo(ECharacterStateType::Attacking, RuntimeData);
		return;
	}

	// ============================================================
	// 优先级 3：松手 → 停止
	// ============================================================

	if (RuntimeData.DesiredWorldMoveDir.IsNearlyZero())
	{
		SM.TryTransitionTo(ECharacterStateType::RunEnd, RuntimeData);
		return;
	}
}

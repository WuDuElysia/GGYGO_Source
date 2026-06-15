/**
 * @file IdleState.cpp
 * @brief 待机状态实现
 *
 * 待机状态下按优先级检查意图：
 * ActionGranted（仲裁结果） > 闪避 > 攻击 > 移动
 *
 * 每个意图都会检查对应的仲裁标记（bBlockMove、bBlockDodge、bBlockAttack）。
 * 仲裁标记由 ArbiterPipeline 在 Tick 开头写入。
 */
#include "StateMachine/State/IdleState.h"
#include "StateMachine/CharacterStateMachine.h"
#include "Data/RuntimeData.h"

void FIdleState::Enter(FRuntimeData& RuntimeData)
{
	// TODO: 阶段八 GAS 接入后同步 GameplayTag（State.Idle）
}

void FIdleState::Update(float DeltaTime, FRuntimeData& RuntimeData, FCharacterStateMachine& SM)
{
	// ============================================================
	// 优先级 0：ActionArbiter 批准的动作（GAS 层面已通过）
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
	// 优先级 2：攻击（备用路径，当 ActionArbiter 未激活时走这里）
	// ============================================================

	if (RuntimeData.bWantsToAttack && !RuntimeData.bBlockAttack)
	{
		SM.TryTransitionTo(ECharacterStateType::Attacking, RuntimeData);
		return;
	}

	// ============================================================
	// 优先级 3：移动 → RunStart
	// ============================================================

	if (!RuntimeData.DesiredWorldMoveDir.IsNearlyZero() && !RuntimeData.bBlockMove)
	{
		SM.TryTransitionTo(ECharacterStateType::RunStart, RuntimeData);
		return;
	}
}

void FIdleState::Exit(FRuntimeData& RuntimeData)
{
	// TODO: 阶段八 GAS 接入后移除 GameplayTag（State.Idle）
}

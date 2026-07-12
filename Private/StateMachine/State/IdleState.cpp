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
#include "StateMachine/GGYGOStateManager.h" // ★ 阶段6：纯C++ 状态管理器
#include "Data/RuntimeData.h"

void FIdleState::Enter(FRuntimeData& RuntimeData)
{
	// TODO: 阶段7 GAS 接入后同步 GameplayTag（State.Idle）
}

void FIdleState::Update(float DeltaTime, FRuntimeData& RuntimeData, FGYGOStateManager& SM)
{
	// ============================================================
	// 优先级 0：ActionArbiter 批准的动作（GAS 层面已通过）
	// ============================================================

	if (RuntimeData.ActionGranted != ECharacterStateType::Idle)
	{
		SM.RequestState(RuntimeData.ActionGranted);
		return;
	}

	// ============================================================
	// 优先级 1：闪避
	// ============================================================

	if (RuntimeData.bWantsToDodge && !RuntimeData.bBlockDodge)
	{
		SM.RequestState(ECharacterStateType::Dodging);
		return;
	}

	// ============================================================
	// 优先级 2：攻击（备用路径，当 ActionArbiter 未激活时走这里）
	// ============================================================

	if (RuntimeData.bWantsToAttack && !RuntimeData.bBlockAttack)
	{
		SM.RequestState(ECharacterStateType::Attacking);
		return;
	}

	// ============================================================
	// 优先级 3：移动 → Moving（速度上限由 MotionDriver 管理）
	// ============================================================

	if (!RuntimeData.DesiredWorldMoveDir.IsNearlyZero() && !RuntimeData.bBlockMove)
	{
		SM.RequestState(ECharacterStateType::Moving);
		return;
	}
}

void FIdleState::Exit(FRuntimeData& RuntimeData)
{
	// TODO: 阶段八 GAS 接入后移除 GameplayTag（State.Idle）
}

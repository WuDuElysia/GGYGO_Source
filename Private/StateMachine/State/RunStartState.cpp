/**
 * @file RunStartState.cpp
 * @brief 跑步启动状态实现
 *
 * 启动动画播放期间：
 * - ActionGranted 优先（仲裁批准的动作可打断启动）
 * - 松手 → 切到 RunEnd（停止动画）
 * - 播够最短时间 → 切到 RunLoop（循环跑）
 */
#include "StateMachine/State/RunStartState.h"
#include "StateMachine/CharacterStateMachine.h"
#include "Data/RuntimeData.h"

void FRunStartState::Enter(FRuntimeData& RuntimeData)
{
	ElapsedTime = 0.f;
	RuntimeData.bRunStartFinished = false;
}

void FRunStartState::Update(float DeltaTime, FRuntimeData& RuntimeData, FCharacterStateMachine& SM)
{
	ElapsedTime += DeltaTime;

	// ============================================================
	// 优先级 0：ActionArbiter 批准的动作（可打断启动动画）
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

	// ============================================================
	// 优先级 4：启动完成 → 循环跑
	// ============================================================

	if (RuntimeData.bRunStartFinished || ElapsedTime >= RuntimeData.RunStartMinDuration)
	{
		SM.TryTransitionTo(ECharacterStateType::RunLoop, RuntimeData);
		return;
	}
}

void FRunStartState::Exit(FRuntimeData& RuntimeData)
{
	RuntimeData.bRunStartFinished = false;
}

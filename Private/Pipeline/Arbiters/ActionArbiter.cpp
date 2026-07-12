/**
 * @file ActionArbiter.cpp
 * @brief 动作优先级仲裁器实现
 */
#include "Pipeline/Arbiters/ActionArbiter.h"
#include "Data/RuntimeData.h"
#include "AbilitySystemComponent.h"
#include "StateMachine/GGYGOStateManager.h"

void FActionArbiter::Init(UAbilitySystemComponent* InASC, FGYGOStateManager* InSM)
{
	ASC = InASC;
	SM = InSM;
}

void FActionArbiter::Arbitrate(FRuntimeData& RuntimeData, float DeltaTime)
{
	if (!ASC) return;

	// 前一层仲裁标记已在 ArbiterPipeline.Process 中重置为 false
	// 如果 GASArbiter 已经拦截（bBlockAttack/bBlockDodge），直接跳过
	const ECharacterStateType Current = RuntimeData.CurrentState;

	// 攻击请求
	if (RuntimeData.bWantsToAttack && !RuntimeData.bBlockAttack)
	{
		if (GetActionPriority(ECharacterStateType::Attacking) > GetStateResistance(Current))
		{
			// ★ 第二道预判：状态机能切过去吗？（查关系矩阵）
			if (SM && !SM->CanEnterState(ECharacterStateType::Attacking))
			{
				// 关系矩阵拒绝（如 Attacking→Dodging=Blocked 时攻击被闪避打断不能立刻反打）
				return;
			}

			// ★ GA 预判（Phase 10 接入）:
			// TODO: if (ASC->TryActivateAbilityByClass(GA_Attack))
			{
				RuntimeData.ActionGranted = ECharacterStateType::Attacking;
			}
		}
	}

	// 闪避请求
	if (RuntimeData.bWantsToDodge && !RuntimeData.bBlockDodge)
	{
		if (GetActionPriority(ECharacterStateType::Dodging) > GetStateResistance(Current))
		{
			if (SM && !SM->CanEnterState(ECharacterStateType::Dodging))
			{
				return;
			}

			// TODO: if (ASC->TryActivateAbilityByClass(GA_Dodge))
			{
				RuntimeData.ActionGranted = ECharacterStateType::Dodging;
			}
		}
	}
}

int32 FActionArbiter::GetStateResistance(ECharacterStateType State)
{
	switch (State)
	{
	case ECharacterStateType::Dodging:    return 80;  // 闪避几乎不可打断
	case ECharacterStateType::Attacking:  return 40;  // 攻击中可被更强动作打断
	case ECharacterStateType::HitStun:    return 0;   // 受击中完全不能动
	case ECharacterStateType::Stunned:    return 0;   // 眩晕中完全不能动
	case ECharacterStateType::Dead:       return 100; // 死亡不可被打断（最高抗性）
	default:                               return 0;   // Idle/Moving/InAir → 无抗性
	}
}

int32 FActionArbiter::GetActionPriority(ECharacterStateType State)
{
	switch (State)
	{
	case ECharacterStateType::Attacking:  return 50;  // 攻击优先级中等
	case ECharacterStateType::Dodging:    return 80;  // 闪避优先级高（用于保命）
	default:                               return 50;
	}
}

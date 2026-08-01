/**
 * @file ActionArbiter.cpp
 * @brief 动作优先级仲裁器（GAS 接入后启用）
 *
 * 当前无 Attacking/Dodging 状态，仲裁器空跑。
 * GAS 接入后在此处实现攻击/闪避的优先级判断。
 */
#include "Pipeline/Arbiters/ActionArbiter.h"
#include "Data/Logic/RuntimeData.h"

void FActionArbiter::Init(UAbilitySystemComponent* InASC, FGYGOStateManager* InSM)
{
	ASC = InASC;
	SM = InSM;
}

void FActionArbiter::Arbitrate(FRuntimeData& RuntimeData, float DeltaTime)
{
	// GAS 接入后启用：攻击/闪避优先级判断 + GA 预判
}

int32 FActionArbiter::GetStateResistance(ECharacterStateType State)
{
	return 0;
}

int32 FActionArbiter::GetActionPriority(ECharacterStateType State)
{
	return 50;
}

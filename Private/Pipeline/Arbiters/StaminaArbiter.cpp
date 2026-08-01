/**
 * @file StaminaArbiter.cpp
 * @brief 体力仲裁器实现
 */
#include "Pipeline/Arbiters/StaminaArbiter.h"
#include "Data/Logic/RuntimeData.h"
#include "AbilitySystemComponent.h"

void FStaminaArbiter::Init(UAbilitySystemComponent* InASC)
{
	ASC = InASC;
}

void FStaminaArbiter::Arbitrate(FRuntimeData& RuntimeData, float DeltaTime)
{
	if (!ASC) return;

	// TODO: 阶段八完成后从 AttributeSet 读取 Stamina 和 MaxStamina：
	// float Stamina    = AttributeSet->GetStamina();
	// float MaxStamina = AttributeSet->GetMaxStamina();
	//
	// if (RuntimeData.bWantsToSprint && !bIsStaminaDepleted)
	// {
	//     消耗体力
	// }
	// else
	// {
	//     恢复体力
	// }
	//
	// 枯竭判定（滞后设计）
	// if (Stamina <= 0.f)
	// {
	//     bIsStaminaDepleted = true;
	// }
	// else if (bIsStaminaDepleted && Stamina >= MaxStamina * StaminaRecoverThreshold)
	// {
	//     bIsStaminaDepleted = false;
	// }
	//
	// if (bIsStaminaDepleted)
	// {
	//     RuntimeData.bWantsToSprint = false;
	// }
}

/**
 * @file HealthArbiter.cpp
 * @brief 血量仲裁器实现
 */
#include "Pipeline/Arbiters/HealthArbiter.h"
#include "Data/Logic/RuntimeData.h"
#include "AbilitySystemComponent.h"

void FHealthArbiter::Init(UAbilitySystemComponent* InASC)
{
	ASC = InASC;
}

void FHealthArbiter::RequestDamage(const FDamageRequest& Request)
{
	if (DamageCount < MaxDamageQueue)
	{
		DamageQueue[DamageCount] = Request;
		++DamageCount;
	}
}

void FHealthArbiter::Arbitrate(FRuntimeData& RuntimeData, float DeltaTime)
{
	if (!ASC) return;

	// 统一结算伤害队列
	for (int32 i = 0; i < DamageCount; ++i)
	{
		// TODO: 阶段八完成后，通过 GE 扣血：
		// ASC->ApplyGameplayEffectToSelf(GE_Damage, DamageQueue[i].Damage, ...)
	}
	DamageCount = 0;

	// TODO: 阶段八完成后，从 AttributeSet 读取 Health 并判断死亡：
	// if (Health <= 0.f)
	// {
	//     RuntimeData.bBlockMove   = true;
	//     RuntimeData.bBlockAttack = true;
	//     RuntimeData.bBlockDodge  = true;
	//     RuntimeData.bBlockInput  = true;
	// }
}

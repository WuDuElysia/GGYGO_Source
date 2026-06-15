/**
 * @file GASArbiter.cpp
 * @brief GAS 状态仲裁器实现
 */
#include "Pipeline/Arbiters/GASArbiter.h"
#include "Data/RuntimeData.h"
#include "AbilitySystemComponent.h"
#include "GameplayTagContainer.h"

void FGASArbiter::Init(UAbilitySystemComponent* InASC)
{
	ASC = InASC;

	// 缓存 Tag，避免每帧字符串查找开销
	Tag_Stunned = FGameplayTag::RequestGameplayTag(FName("State.Stunned"));
	Tag_Dead    = FGameplayTag::RequestGameplayTag(FName("State.Dead"));
}

void FGASArbiter::Arbitrate(FRuntimeData& RuntimeData, float DeltaTime)
{
	if (!ASC) return;

	// 死亡 > 眩晕：按优先级单独判断，死亡覆盖面最广
	if (ASC->HasMatchingGameplayTag(Tag_Dead))
	{
		RuntimeData.bBlockMove   = true;
		RuntimeData.bBlockAttack = true;
		RuntimeData.bBlockDodge  = true;
		RuntimeData.bBlockInput  = true;
		return; // 死亡是最高优先级，不再检查其他
	}

	if (ASC->HasMatchingGameplayTag(Tag_Stunned))
	{
		RuntimeData.bBlockMove   = true;
		RuntimeData.bBlockAttack = true;
		RuntimeData.bBlockDodge  = true;
		// 眩晕不阻断输入处理，只是禁止动作执行
	}
}

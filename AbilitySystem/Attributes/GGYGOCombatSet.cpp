/**
 * @file GGYGOCombatSet.cpp
 * @brief 输出侧属性集实现
 */
#include "AbilitySystem/Attributes/GGYGOCombatSet.h"

#include "Net/UnrealNetwork.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCombatSet)

UGGYGOCombatSet::UGGYGOCombatSet()
	: BaseDamage(0.0f)
	, BaseHeal(0.0f)
	, BasePoiseDamage(0.0f)
{
	// 全部从 0 起步。真实数值由角色的初始化 GE 或 AbilitySet 设定，
	// 避免在 C++ 里硬编码数值平衡。
}

void UGGYGOCombatSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// COND_OwnerOnly：别人的攻击力对本地客户端没有意义，伤害结算在服务器完成。
	// 只有拥有者需要这些值（用于 UI 显示自己的面板数值）。
	DOREPLIFETIME_CONDITION_NOTIFY(UGGYGOCombatSet, BaseDamage, COND_OwnerOnly, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UGGYGOCombatSet, BaseHeal, COND_OwnerOnly, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UGGYGOCombatSet, BasePoiseDamage, COND_OwnerOnly, REPNOTIFY_Always);
}

void UGGYGOCombatSet::OnRep_BaseDamage(const FGameplayAttributeData& OldValue)
{
	// 只把复制值交回 GAS 聚合器，不在客户端重跑伤害公式。
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGGYGOCombatSet, BaseDamage, OldValue);
}

void UGGYGOCombatSet::OnRep_BaseHeal(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGGYGOCombatSet, BaseHeal, OldValue);
}

void UGGYGOCombatSet::OnRep_BasePoiseDamage(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGGYGOCombatSet, BasePoiseDamage, OldValue);
}

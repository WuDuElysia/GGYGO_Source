/**
 * @file GGYGOAttributeSet.cpp
 * @brief AttributeSet 基类实现
 */
#include "AbilitySystem/Attributes/GGYGOAttributeSet.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAttributeSet)

class UWorld;

UGGYGOAttributeSet::UGGYGOAttributeSet()
{
	// 基类不持有属性，派生 Set 在各自构造函数里设默认值。
}

UWorld* UGGYGOAttributeSet::GetWorld() const
{
	// AttributeSet 是 ASC 的子对象，不是 Actor，所以要沿 Outer 链取 World。
	// HealthSet 广播伤害消息时需要 World 来拿 GameplayMessageSubsystem。
	const UObject* Outer = GetOuter();
	check(Outer);

	return Outer->GetWorld();
}

UGGYGOAbilitySystemComponent* UGGYGOAttributeSet::GetGGYGOAbilitySystemComponent() const
{
	return Cast<UGGYGOAbilitySystemComponent>(GetOwningAbilitySystemComponent());
}

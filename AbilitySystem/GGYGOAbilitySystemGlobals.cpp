/**
 * @file GGYGOAbilitySystemGlobals.cpp
 * @brief 项目级 AbilitySystemGlobals 实现
 */
#include "AbilitySystem/GGYGOAbilitySystemGlobals.h"

#include "AbilitySystem/GGYGOGameplayEffectContext.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAbilitySystemGlobals)

struct FGameplayEffectContext;

UGGYGOAbilitySystemGlobals::UGGYGOAbilitySystemGlobals(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 只建立 UObject 生命周期。这里不能分配 EffectContext，也不做任何 Ability / GE 初始化。
}

FGameplayEffectContext* UGGYGOAbilitySystemGlobals::AllocGameplayEffectContext() const
{
	// 返回自定义类型而非引擎默认类型，使 MakeEffectContext、TargetData 与网络复制共用同一扩展上下文。
	return new FGGYGOGameplayEffectContext();
}

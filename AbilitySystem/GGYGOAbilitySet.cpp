/**
 * @file GGYGOAbilitySet.cpp
 * @brief AbilitySet 授予与回收实现
 */
#include "AbilitySystem/GGYGOAbilitySet.h"

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAbilitySet)

void FGGYGOAbilitySet_GrantedHandles::AddAbilitySpecHandle(const FGameplayAbilitySpecHandle& Handle)
{
	if (Handle.IsValid())
	{
		AbilitySpecHandles.Add(Handle);
	}
}

void FGGYGOAbilitySet_GrantedHandles::AddGameplayEffectHandle(const FActiveGameplayEffectHandle& Handle)
{
	if (Handle.IsValid())
	{
		GameplayEffectHandles.Add(Handle);
	}
}

void FGGYGOAbilitySet_GrantedHandles::AddAttributeSet(UAttributeSet* Set)
{
	GrantedAttributeSets.Add(Set);
}

void FGGYGOAbilitySet_GrantedHandles::TakeFromAbilitySystem(UGGYGOAbilitySystemComponent* GGYGOASC)
{
	check(GGYGOASC);

	if (!GGYGOASC->IsOwnerActorAuthoritative())
	{
		// 只有服务器能改 AbilitySpec、活动 GE 和挂载的属性集。
		// 注意这里**不清空**本地句柄：客户端保留记录，避免误认为已经回收过了。
		return;
	}

	// 顺序与授予相反。先清能力，让依赖这些能力的激活先结束，
	// 再移除效果，最后摘属性集——否则可能出现能力还在跑但属性存储已经没了。
	for (const FGameplayAbilitySpecHandle& Handle : AbilitySpecHandles)
	{
		if (Handle.IsValid())
		{
			GGYGOASC->ClearAbility(Handle);
		}
	}

	for (const FActiveGameplayEffectHandle& Handle : GameplayEffectHandles)
	{
		if (Handle.IsValid())
		{
			GGYGOASC->RemoveActiveGameplayEffect(Handle);
		}
	}

	for (UAttributeSet* Set : GrantedAttributeSets)
	{
		GGYGOASC->RemoveSpawnedAttribute(Set);
	}

	AbilitySpecHandles.Reset();
	GameplayEffectHandles.Reset();
	GrantedAttributeSets.Reset();
}

UGGYGOAbilitySet::UGGYGOAbilitySet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 纯配置资产，构造时不做任何授予。
}

void UGGYGOAbilitySet::GiveToAbilitySystem(UGGYGOAbilitySystemComponent* GGYGOASC, FGGYGOAbilitySet_GrantedHandles* OutGrantedHandles, UObject* SourceObject) const
{
	check(GGYGOASC);

	if (!GGYGOASC->IsOwnerActorAuthoritative())
	{
		// 授予必须在服务器进行，客户端靠复制拿到结果。
		return;
	}

	// 第一步：属性集。必须最先，因为下面的能力和 GE 都可能读写属性。
	for (int32 SetIndex = 0; SetIndex < GrantedAttributes.Num(); ++SetIndex)
	{
		const FGGYGOAbilitySet_AttributeSet& SetToGrant = GrantedAttributes[SetIndex];

		if (!IsValid(SetToGrant.AttributeSet))
		{
			// 单项配置错误不中断其余授予，只记录足够定位的信息。
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("AbilitySet [%s] 的 GrantedAttributes[%d] 无效，已跳过。"),
				*GetNameSafe(this), SetIndex);
			continue;
		}

		// Outer 用 ASC 的 Owner，让属性集实例跟随拥有者的生命周期。
		UAttributeSet* NewSet = NewObject<UAttributeSet>(GGYGOASC->GetOwner(), SetToGrant.AttributeSet);
		GGYGOASC->AddAttributeSetSubobject(NewSet);

		if (OutGrantedHandles)
		{
			OutGrantedHandles->AddAttributeSet(NewSet);
		}
	}

	// 第二步：能力。
	for (int32 AbilityIndex = 0; AbilityIndex < GrantedGameplayAbilities.Num(); ++AbilityIndex)
	{
		const FGGYGOAbilitySet_GameplayAbility& AbilityToGrant = GrantedGameplayAbilities[AbilityIndex];

		if (!IsValid(AbilityToGrant.Ability))
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("AbilitySet [%s] 的 GrantedGameplayAbilities[%d] 无效，已跳过。"),
				*GetNameSafe(this), AbilityIndex);
			continue;
		}

		// 用 CDO 作为 Spec 模板。CDO 是共享的类默认对象，不是运行时实例。
		UGGYGOGameplayAbility* AbilityCDO = AbilityToGrant.Ability->GetDefaultObject<UGGYGOGameplayAbility>();

		FGameplayAbilitySpec AbilitySpec(AbilityCDO, AbilityToGrant.AbilityLevel);
		AbilitySpec.SourceObject = SourceObject;

		// 输入 Tag 写进动态源标签，ASC 的输入分发靠精确匹配它来找到这个 Spec。
		if (AbilityToGrant.InputTag.IsValid())
		{
			AbilitySpec.GetDynamicSpecSourceTags().AddTag(AbilityToGrant.InputTag);
		}

		const FGameplayAbilitySpecHandle AbilitySpecHandle = GGYGOASC->GiveAbility(AbilitySpec);

		if (OutGrantedHandles)
		{
			OutGrantedHandles->AddAbilitySpecHandle(AbilitySpecHandle);
		}
	}

	// 第三步：效果。通常是属性初始化 GE，此时属性集已经就位。
	for (int32 EffectIndex = 0; EffectIndex < GrantedGameplayEffects.Num(); ++EffectIndex)
	{
		const FGGYGOAbilitySet_GameplayEffect& EffectToGrant = GrantedGameplayEffects[EffectIndex];

		if (!IsValid(EffectToGrant.GameplayEffect))
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("AbilitySet [%s] 的 GrantedGameplayEffects[%d] 无效，已跳过。"),
				*GetNameSafe(this), EffectIndex);
			continue;
		}

		const UGameplayEffect* GameplayEffect = EffectToGrant.GameplayEffect->GetDefaultObject<UGameplayEffect>();
		const FActiveGameplayEffectHandle GameplayEffectHandle = GGYGOASC->ApplyGameplayEffectToSelf(
			GameplayEffect,
			EffectToGrant.EffectLevel,
			GGYGOASC->MakeEffectContext());

		if (OutGrantedHandles)
		{
			OutGrantedHandles->AddGameplayEffectHandle(GameplayEffectHandle);
		}
	}
}

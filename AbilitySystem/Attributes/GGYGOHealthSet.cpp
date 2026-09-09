/**
 * @file GGYGOHealthSet.cpp
 * @brief 承受侧属性集实现
 */
#include "AbilitySystem/Attributes/GGYGOHealthSet.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Engine/World.h"
#include "GameFramework/GameplayMessageSubsystem.h"
#include "GameplayEffectExtension.h"
#include "Messages/GGYGOVerbMessage.h"
#include "Net/UnrealNetwork.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOHealthSet)

UGGYGOHealthSet::UGGYGOHealthSet()
	: Health(100.0f)
	, MaxHealth(100.0f)
	, Poise(100.0f)
	, MaxPoise(100.0f)
{
	bOutOfHealth = false;
	bPoiseBroken = false;

	// 快照先归零；第一次通过 Pre 阶段的 GE 会写入真实旧值。
	HealthBeforeAttributeChange = 0.0f;
	MaxHealthBeforeAttributeChange = 0.0f;
	PoiseBeforeAttributeChange = 0.0f;
}

void UGGYGOHealthSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// 只复制持久状态。三个元属性是一次性输入，复制它们没有意义且会浪费带宽。
	// REPNOTIFY_Always 保证即使数值相同也走 OnRep，让客户端有机会重新广播表现事件。
	DOREPLIFETIME_CONDITION_NOTIFY(UGGYGOHealthSet, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UGGYGOHealthSet, MaxHealth, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UGGYGOHealthSet, Poise, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UGGYGOHealthSet, MaxPoise, COND_None, REPNOTIFY_Always);
}

void UGGYGOHealthSet::OnRep_Health(const FGameplayAttributeData& OldValue)
{
	// 必须先交回 GAS 处理复制值与聚合器，否则属性状态会不同步。
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGGYGOHealthSet, Health, OldValue);

	const float CurrentHealth = GetHealth();

	// 这是网络快照差值，不等于某一次 GE 的原始幅度（两帧之间可能发生多次修改）。
	const float EstimatedMagnitude = CurrentHealth - OldValue.GetCurrentValue();

	// 客户端没有 EffectSpec，来源三参数只能传 nullptr。监听方必须判空。
	OnHealthChanged.Broadcast(nullptr, nullptr, nullptr, EstimatedMagnitude, OldValue.GetCurrentValue(), CurrentHealth);

	if (!bOutOfHealth && CurrentHealth <= 0.0f)
	{
		OnOutOfHealth.Broadcast(nullptr, nullptr, nullptr, EstimatedMagnitude, OldValue.GetCurrentValue(), CurrentHealth);
	}

	// 广播之后再更新边沿，保证同一次跨零只触发一次。
	bOutOfHealth = (CurrentHealth <= 0.0f);
}

void UGGYGOHealthSet::OnRep_MaxHealth(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGGYGOHealthSet, MaxHealth, OldValue);

	OnMaxHealthChanged.Broadcast(nullptr, nullptr, nullptr, GetMaxHealth() - OldValue.GetCurrentValue(), OldValue.GetCurrentValue(), GetMaxHealth());
}

void UGGYGOHealthSet::OnRep_Poise(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGGYGOHealthSet, Poise, OldValue);

	const float CurrentPoise = GetPoise();
	const float EstimatedMagnitude = CurrentPoise - OldValue.GetCurrentValue();

	OnPoiseChanged.Broadcast(nullptr, nullptr, nullptr, EstimatedMagnitude, OldValue.GetCurrentValue(), CurrentPoise);

	if (!bPoiseBroken && CurrentPoise <= 0.0f)
	{
		OnPoiseBroken.Broadcast(nullptr, nullptr, nullptr, EstimatedMagnitude, OldValue.GetCurrentValue(), CurrentPoise);
	}

	bPoiseBroken = (CurrentPoise <= 0.0f);
}

void UGGYGOHealthSet::OnRep_MaxPoise(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UGGYGOHealthSet, MaxPoise, OldValue);
}

bool UGGYGOHealthSet::PreGameplayEffectExecute(FGameplayEffectModCallbackData& Data)
{
	if (!Super::PreGameplayEffectExecute(Data))
	{
		return false;
	}

	// 只拦截正向伤害。负值不当作"伤害"处理，避免用负伤害绕过免疫来治疗。
	if (Data.EvaluatedData.Attribute == GetDamageAttribute() && Data.EvaluatedData.Magnitude > 0.0f)
	{
		// 自毁/处死类伤害绕过免疫与开发期保命规则。
		const bool bIsDamageFromSelfDestruct = Data.EffectSpec.GetDynamicAssetTags().HasTagExact(GGYGOGameplayTags::Gameplay_Damage_SelfDestruct);

		// 闪避无敌帧就是靠这个 Tag 生效的。
		if (Data.Target.HasMatchingGameplayTag(GGYGOGameplayTags::Gameplay_Damage_Immunity) && !bIsDamageFromSelfDestruct)
		{
			Data.EvaluatedData.Magnitude = 0.0f;
			return false;
		}

#if !UE_BUILD_SHIPPING
		if (Data.Target.HasMatchingGameplayTag(GGYGOGameplayTags::Cheat_GodMode) && !bIsDamageFromSelfDestruct)
		{
			Data.EvaluatedData.Magnitude = 0.0f;
			return false;
		}
#endif
	}

	// 削韧同样受免疫约束：无敌帧期间不该被削韧。
	if (Data.EvaluatedData.Attribute == GetPoiseDamageAttribute() && Data.EvaluatedData.Magnitude > 0.0f)
	{
		if (Data.Target.HasMatchingGameplayTag(GGYGOGameplayTags::Gameplay_Damage_Immunity))
		{
			Data.EvaluatedData.Magnitude = 0.0f;
			return false;
		}
	}

	// 只有确定要应用的 GE 才存快照。
	HealthBeforeAttributeChange = GetHealth();
	MaxHealthBeforeAttributeChange = GetMaxHealth();
	PoiseBeforeAttributeChange = GetPoise();

	return true;
}

void UGGYGOHealthSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);

	const bool bIsDamageFromSelfDestruct = Data.EffectSpec.GetDynamicAssetTags().HasTagExact(GGYGOGameplayTags::Gameplay_Damage_SelfDestruct);

	// 普通情况下生命值可以被打到 0。
	float MinimumHealth = 0.0f;

#if !UE_BUILD_SHIPPING
	// 开发期保命：作弊 Tag 下最低保留 1 点血，但自毁伤害仍可真正致死。
	if (!bIsDamageFromSelfDestruct &&
		(Data.Target.HasMatchingGameplayTag(GGYGOGameplayTags::Cheat_GodMode) || Data.Target.HasMatchingGameplayTag(GGYGOGameplayTags::Cheat_UnlimitedHealth)))
	{
		MinimumHealth = 1.0f;
	}
#endif

	// 服务器路径可以拿到完整来源上下文。
	const FGameplayEffectContextHandle& EffectContext = Data.EffectSpec.GetEffectContext();
	AActor* Instigator = EffectContext.GetOriginalInstigator();
	AActor* Causer = EffectContext.GetEffectCauser();

	if (Data.EvaluatedData.Attribute == GetDamageAttribute())
	{
		// 先广播消息再扣血。消息里的 Magnitude 是未 Clamp 的原始伤害，
		// 因此伤害数字可能大于目标实际损失的生命值（残血被一击打死的情况）。
		if (Data.EvaluatedData.Magnitude > 0.0f)
		{
			FGGYGOVerbMessage Message;
			Message.Verb = GGYGOGameplayTags::Message_Damage;
			// 这里用 EffectCauser 而不是 OriginalInstigator：表现层关心的是"什么东西打的"。
			Message.Instigator = Causer;
			Message.InstigatorTags = *Data.EffectSpec.CapturedSourceTags.GetAggregatedTags();
			Message.Target = GetOwningActor();
			Message.TargetTags = *Data.EffectSpec.CapturedTargetTags.GetAggregatedTags();
			Message.Magnitude = Data.EvaluatedData.Magnitude;

			UGameplayMessageSubsystem& MessageSystem = UGameplayMessageSubsystem::Get(GetWorld());
			MessageSystem.BroadcastMessage(Message.Verb, Message);
		}

		// 转成 -Health 并 Clamp，然后**必须清零**，否则后续 GE 会重复消费同一份伤害。
		SetHealth(FMath::Clamp(GetHealth() - GetDamage(), MinimumHealth, GetMaxHealth()));
		SetDamage(0.0f);
	}
	else if (Data.EvaluatedData.Attribute == GetHealingAttribute())
	{
		SetHealth(FMath::Clamp(GetHealth() + GetHealing(), MinimumHealth, GetMaxHealth()));
		SetHealing(0.0f);
	}
	else if (Data.EvaluatedData.Attribute == GetPoiseDamageAttribute())
	{
		if (Data.EvaluatedData.Magnitude > 0.0f)
		{
			FGGYGOVerbMessage Message;
			Message.Verb = GGYGOGameplayTags::Message_PoiseBreak;
			Message.Instigator = Causer;
			Message.InstigatorTags = *Data.EffectSpec.CapturedSourceTags.GetAggregatedTags();
			Message.Target = GetOwningActor();
			Message.TargetTags = *Data.EffectSpec.CapturedTargetTags.GetAggregatedTags();
			Message.Magnitude = Data.EvaluatedData.Magnitude;

			UGameplayMessageSubsystem& MessageSystem = UGameplayMessageSubsystem::Get(GetWorld());
			MessageSystem.BroadcastMessage(Message.Verb, Message);
		}

		SetPoise(FMath::Clamp(GetPoise() - GetPoiseDamage(), 0.0f, GetMaxPoise()));
		SetPoiseDamage(0.0f);
	}
	else if (Data.EvaluatedData.Attribute == GetHealthAttribute())
	{
		// 外部直接改 Health 时同样 Clamp，然后落到下面的边沿判定。
		SetHealth(FMath::Clamp(GetHealth(), MinimumHealth, GetMaxHealth()));
	}
	else if (Data.EvaluatedData.Attribute == GetPoiseAttribute())
	{
		SetPoise(FMath::Clamp(GetPoise(), 0.0f, GetMaxPoise()));
	}
	else if (Data.EvaluatedData.Attribute == GetMaxHealthAttribute())
	{
		// 当前 Health 超出新上限的压低由 PostAttributeChange 完成，这里只广播上限变化。
		OnMaxHealthChanged.Broadcast(Instigator, Causer, &Data.EffectSpec, Data.EvaluatedData.Magnitude, MaxHealthBeforeAttributeChange, GetMaxHealth());
	}

	// 只有真实变化才广播，避免 UI 收到无意义刷新。
	if (GetHealth() != HealthBeforeAttributeChange)
	{
		OnHealthChanged.Broadcast(Instigator, Causer, &Data.EffectSpec, Data.EvaluatedData.Magnitude, HealthBeforeAttributeChange, GetHealth());
	}

	if (GetPoise() != PoiseBeforeAttributeChange)
	{
		OnPoiseChanged.Broadcast(Instigator, Causer, &Data.EffectSpec, Data.EvaluatedData.Magnitude, PoiseBeforeAttributeChange, GetPoise());
	}

	if ((GetHealth() <= 0.0f) && !bOutOfHealth)
	{
		OnOutOfHealth.Broadcast(Instigator, Causer, &Data.EffectSpec, Data.EvaluatedData.Magnitude, HealthBeforeAttributeChange, GetHealth());
	}

	if ((GetPoise() <= 0.0f) && !bPoiseBroken)
	{
		OnPoiseBroken.Broadcast(Instigator, Causer, &Data.EffectSpec, Data.EvaluatedData.Magnitude, PoiseBeforeAttributeChange, GetPoise());
	}

	// 监听方可能在广播过程中又改了属性（例如破韧监听者立刻施加硬直 GE），
	// 所以边沿状态要在所有广播之后重新读取，不能用上面的旧值。
	bOutOfHealth = (GetHealth() <= 0.0f);
	bPoiseBroken = (GetPoise() <= 0.0f);
}

void UGGYGOHealthSet::PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const
{
	Super::PreAttributeBaseChange(Attribute, NewValue);

	ClampAttribute(Attribute, NewValue);
}

void UGGYGOHealthSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);

	ClampAttribute(Attribute, NewValue);
}

void UGGYGOHealthSet::PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue)
{
	Super::PostAttributeChange(Attribute, OldValue, NewValue);

	// 上限下调时必须同步压低当前值，否则会出现 Health 大于 MaxHealth。
	// 走 ASC 的 ApplyModToAttribute 而不是直接 SetHealth，是为了让这次修改仍然经过
	// 完整的属性变更生命周期与通知。
	if (Attribute == GetMaxHealthAttribute())
	{
		if (GetHealth() > NewValue)
		{
			UGGYGOAbilitySystemComponent* GGYGOASC = GetGGYGOAbilitySystemComponent();
			check(GGYGOASC);

			GGYGOASC->ApplyModToAttribute(GetHealthAttribute(), EGameplayModOp::Override, NewValue);
		}
	}
	else if (Attribute == GetMaxPoiseAttribute())
	{
		if (GetPoise() > NewValue)
		{
			UGGYGOAbilitySystemComponent* GGYGOASC = GetGGYGOAbilitySystemComponent();
			check(GGYGOASC);

			GGYGOASC->ApplyModToAttribute(GetPoiseAttribute(), EGameplayModOp::Override, NewValue);
		}
	}

	// 复活或韧性恢复后解锁边沿，使下一次归零能重新广播。
	if (bOutOfHealth && (GetHealth() > 0.0f))
	{
		bOutOfHealth = false;
	}

	if (bPoiseBroken && (GetPoise() > 0.0f))
	{
		bPoiseBroken = false;
	}
}

void UGGYGOHealthSet::ClampAttribute(const FGameplayAttribute& Attribute, float& NewValue) const
{
	if (Attribute == GetHealthAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxHealth());
	}
	else if (Attribute == GetMaxHealthAttribute())
	{
		// 上限至少 1，否则 Health 的 Clamp 区间会退化成 [0, 0]。
		NewValue = FMath::Max(NewValue, 1.0f);
	}
	else if (Attribute == GetPoiseAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxPoise());
	}
	else if (Attribute == GetMaxPoiseAttribute())
	{
		NewValue = FMath::Max(NewValue, 1.0f);
	}
}

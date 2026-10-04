/**
 * @file GGYGOAbilitySet.cpp
 * @brief AbilitySet 授予与回收实现
 */
#include "AbilitySystem/GGYGOAbilitySet.h"

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "GameFramework/Actor.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAbilitySet)

namespace
{
	// G1用于编辑校验，后续运行准入复用；非编辑构建暂未接入调用，保持文件内无状态接口。
	bool IsValidAttributeSetClass(UClass* SetClass)
	{
		return IsValid(SetClass) && SetClass->IsChildOf(UAttributeSet::StaticClass());
	}

	[[maybe_unused]] bool IsGrantableAttributeSetClass(UClass* SetClass)
	{
		return IsValidAttributeSetClass(SetClass)
			&& !SetClass->HasAnyClassFlags(CLASS_Abstract);
	}

	[[maybe_unused]] bool HaveAttributeSetClassConflict(UClass* FirstClass, UClass* SecondClass, FString& OutReason)
	{
		OutReason.Reset();
		// 已有存储按类型比较；abstract标记限制新建资格，不能令其在冲突判断中消失。
		if (!IsValidAttributeSetClass(FirstClass) || !IsValidAttributeSetClass(SecondClass))
		{
			return false; // 无效类由调用方单独诊断，不归类为两个存储间的冲突。
		}
		if (FirstClass == SecondClass)
		{
			OutReason = TEXT("同一属性集类重复配置");
			return true;
		}
		if (FirstClass->IsChildOf(SecondClass) || SecondClass->IsChildOf(FirstClass))
		{
			OutReason = TEXT("父子继承关系导致属性集查找不唯一");
			return true;
		}

		TArray<FGameplayAttribute> FirstAttributes;
		TArray<FGameplayAttribute> SecondAttributes;
		UAttributeSet::GetAttributesFromSetClass(FirstClass, FirstAttributes);
		UAttributeSet::GetAttributesFromSetClass(SecondClass, SecondAttributes);
		for (const FGameplayAttribute& Attribute : FirstAttributes)
		{
			// 引擎相等比较字段身份；Sibling的继承字段相同，独立声明的同名字段不同。
			if (SecondAttributes.Contains(Attribute))
			{
				OutReason = FString::Printf(TEXT("共享属性字段 [%s.%s] 导致重复存储"),
					*GetNameSafe(Attribute.GetAttributeSetClass()), *Attribute.GetName());
				return true;
			}
		}
		return false;
	}
}

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

#if WITH_EDITOR
EDataValidationResult UGGYGOAbilitySet::IsDataValid(FDataValidationContext& Context) const
{
	const EDataValidationResult SuperResult = Super::IsDataValid(Context);
	bool bHasInvalidAttributes = false;
	for (int32 SetIndex = 0; SetIndex < GrantedAttributes.Num(); ++SetIndex)
	{
		UClass* SetClass = GrantedAttributes[SetIndex].AttributeSet.Get();
		if (!IsGrantableAttributeSetClass(SetClass))
		{
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("GrantedAttributes[%d] 的属性集类 [%s] 为空、无效或抽象。"),
				SetIndex, *GetNameSafe(SetClass))));
			bHasInvalidAttributes = true;
			continue;
		}
		for (int32 PreviousIndex = 0; PreviousIndex < SetIndex; ++PreviousIndex)
		{
			UClass* PreviousClass = GrantedAttributes[PreviousIndex].AttributeSet.Get();
			FString ConflictReason;
			if (HaveAttributeSetClassConflict(PreviousClass, SetClass, ConflictReason))
			{
				Context.AddError(FText::FromString(FString::Printf(
					TEXT("GrantedAttributes[%d] [%s] 与 GrantedAttributes[%d] [%s] 冲突：%s。"),
					PreviousIndex, *GetNameSafe(PreviousClass), SetIndex, *GetNameSafe(SetClass), *ConflictReason)));
				bHasInvalidAttributes = true;
			}
		}
	}
	return bHasInvalidAttributes || SuperResult == EDataValidationResult::Invalid
		? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}
#endif

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
		UClass* SetClass = SetToGrant.AttributeSet.Get();

		if (!IsGrantableAttributeSetClass(SetClass))
		{
			// 单项配置错误不中断其余授予，只记录足够定位的信息。
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("AbilitySet [%s] 的 GrantedAttributes[%d] 类 [%s] 为空、无效或抽象，已跳过。"),
				*GetNameSafe(this), SetIndex, *GetNameSafe(SetClass));
			continue;
		}

		// 固定本项的Actor Outer；构造后仍须是同一有效权威宿主。
		AActor* GrantOwner = IsValid(GGYGOASC) ? GGYGOASC->GetOwner() : nullptr;
		const auto IsGrantContextValid = [GGYGOASC, GrantOwner]()
		{
			return IsValid(GGYGOASC) && !GGYGOASC->IsBeingDestroyed()
				&& GGYGOASC->IsOwnerActorAuthoritative()
				&& IsValid(GrantOwner) && !GrantOwner->IsActorBeingDestroyed()
				&& GrantOwner->HasAuthority() && GGYGOASC->GetOwner() == GrantOwner;
		};
		if (!IsGrantContextValid())
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("AbilitySet [%s] 的 GrantedAttributes[%d] [%s] 缺少有效权威ASC/Actor Outer，已跳过。"),
				*GetNameSafe(this), SetIndex, *GetNameSafe(SetClass));
			continue;
		}

		const auto HasStorageConflict = [this, GGYGOASC, SetIndex, SetClass]()
		{
			// 每次调用重新读取ASC权威列表，不沿用构造前快照或忽略abstract已有类。
			for (UAttributeSet* ExistingSet : GGYGOASC->GetSpawnedAttributes())
			{
				FString ConflictReason;
				if (ExistingSet && HaveAttributeSetClassConflict(SetClass, ExistingSet->GetClass(), ConflictReason))
				{
					UE_LOG(LogGGYGOAbilitySystem, Error,
						TEXT("AbilitySet [%s] 的 GrantedAttributes[%d] [%s] 与ASC已有属性集 [%s] 类 [%s] 冲突：%s，已跳过。"),
						*GetNameSafe(this), SetIndex, *GetNameSafe(SetClass), *GetNameSafe(ExistingSet),
						*GetNameSafe(ExistingSet->GetClass()), *ConflictReason);
					return true;
				}
			}
			return false;
		};
		if (HasStorageConflict())
		{
			continue;
		}

		UAttributeSet* NewSet = NewObject<UAttributeSet>(GrantOwner, SetClass);
		if (!IsGrantContextValid() || !IsValid(NewSet) || NewSet->GetOuter() != GrantOwner)
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("AbilitySet [%s] 的 GrantedAttributes[%d] [%s] 构造后实例或原权威Outer失效，已跳过挂接。"),
				*GetNameSafe(this), SetIndex, *GetNameSafe(SetClass));
			continue;
		}
		if (HasStorageConflict())
		{
			// 未挂接的新对象交GC；既有存储不借用、不移除、不记入本次句柄。
			continue;
		}

		GGYGOASC->AddAttributeSetSubobject(NewSet);
		if (!GGYGOASC->GetSpawnedAttributes().Contains(NewSet))
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("AbilitySet [%s] 的 GrantedAttributes[%d] [%s] 未实际挂接，未记录回收句柄。"),
				*GetNameSafe(this), SetIndex, *GetNameSafe(SetClass));
			continue;
		}

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

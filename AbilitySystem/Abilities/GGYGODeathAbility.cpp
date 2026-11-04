/**
 * @file GGYGODeathAbility.cpp
 * @brief 死亡演出能力实现
 */
#include "AbilitySystem/Abilities/GGYGODeathAbility.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "Character/Components/GGYGOHealthComponent.h"
#include "System/GGYGOGameplayTags.h"
#include "Animation/AnimMontage.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGODeathAbility)

UGGYGODeathAbility::UGGYGODeathAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 死亡由服务器判定；GAS 把激活复制给客户端，客户端只播放复制来的 Montage。
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerInitiated;

	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		FAbilityTriggerData Trigger;
		Trigger.TriggerTag = GGYGOGameplayTags::Event_Death;
		Trigger.TriggerSource = EGameplayAbilityTriggerSource::GameplayEvent;
		AbilityTriggers.Add(Trigger);
	}
}

void UGGYGODeathAbility::ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	if (bHasBlueprintActivate || bHasBlueprintActivateFromEvent)
	{
		Super::ActivateAbilityBody(Original, Handle, ActorInfo, ActivationInfo, TriggerEventData);
		return;
	}

	if (HasAuthority(&ActivationInfo))
	{
		UGGYGOHealthComponent* Health = UGGYGOHealthComponent::FindHealthComponent(GetAvatarActorFromActorInfo());
		if (!Health)
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("DeathAbility [%s]: Avatar [%s] 没有 HealthComponent，无法推进死亡，能力以取消结束。"),
				*GetPathName(), *GetPathNameSafe(GetAvatarActorFromActorInfo()));
			RequestAbilityEnd(Original, true, true);
			return;
		}
		// 倒下期间其余能力不再继续；声明 SurvivesDeath 的能力（死亡演出本身这类）保留。
		if (UGGYGOAbilitySystemComponent* ASC = GetGGYGOAbilitySystemComponentFromActorInfo())
		{
			ASC->CancelAbilitiesByFunc([this](const UGGYGOGameplayAbility* Ability, FGameplayAbilitySpecHandle)
			{
				return Ability != this && !Ability->GetAssetTags().HasTag(GGYGOGameplayTags::Ability_Behavior_SurvivesDeath);
			}, /*bReplicateCancelAbility=*/true);
			if (!IsActivationCurrent(Original)) { return; }
		}
		DyingHealth = Health;
		Health->StartDeath();
		if (!IsActivationCurrent(Original)) { return; }
	}

	if (!DeathMontage)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("DeathAbility [%s]: 未配置 DeathMontage，不播放倒下动作，立即结束死亡。"), *GetPathName());
		RequestAbilityEnd(Original, true, false);
		return;
	}

	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = UGGYGOAbilityTask_PlayMontageAndWaitForEvent::PlayMontageAndWaitForEvent(
		this, TEXT("Death"), DeathMontage, FGameplayTagContainer());
	const TWeakObjectPtr<UGGYGODeathAbility> WeakThis(this);
	const auto MakeEnd = [WeakThis, Original](bool bWasCancelled)
	{
		FGGYGOPlayMontageAndWaitForEventDelegate Callback;
		Callback.BindLambda([WeakThis, Original, bWasCancelled](FGameplayTag, FGameplayEventData)
		{
			if (UGGYGODeathAbility* Ability = WeakThis.Get())
			{
				Ability->RequestAbilityEnd(Original, true, bWasCancelled);
			}
		});
		return Callback;
	};
	// 淡出一开始就算倒下完成：再等淡出结束，角色会在消失前被混回站立姿态。
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbacks Callbacks;
	Callbacks.OnBlendOut = MakeEnd(false);
	Callbacks.OnCompleted = MakeEnd(false);
	Callbacks.OnInterrupted = MakeEnd(true);
	Callbacks.OnCancelled = MakeEnd(true);
	Callbacks.OnFailed.BindLambda([WeakThis, Original](const FGGYGOMontageTaskFailureFact&)
	{
		if (UGGYGODeathAbility* Ability = WeakThis.Get())
		{
			Ability->RequestAbilityEnd(Original, true, true);
		}
	});
	if (!Task || !Task->RegisterNativeCallbacks(MoveTemp(Callbacks)).IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("DeathAbility [%s]: 无法创建 Montage 任务或登记回调，立即结束死亡。"), *GetPathName());
		RequestAbilityEnd(Original, true, true);
		return;
	}
	Task->ReadyForActivation();
}

void UGGYGODeathAbility::CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context)
{
	Super::CleanupAbilityResourcesForTermination(Context);
	// 被取消（例如 Avatar 释放）也结束死亡，不留在 Dying。FinishDeath 的广播会同步换人；
	// 此时本次激活已不是当前激活，换人的能力决定会跳过它。
	UGGYGOHealthComponent* Health = DyingHealth.Get();
	DyingHealth.Reset();
	if (Health)
	{
		Health->FinishDeath();
	}
}

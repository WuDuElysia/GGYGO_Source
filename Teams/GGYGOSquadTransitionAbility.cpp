/**
 * @file GGYGOSquadTransitionAbility.cpp
 * @brief 队员进场／退场能力基类实现
 */
#include "Teams/GGYGOSquadTransitionAbility.h"

#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "Animation/AnimMontage.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOSquadTransitionAbility)

UGGYGOSquadTransitionAbility::UGGYGOSquadTransitionAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerInitiated;
}

void UGGYGOSquadTransitionAbility::ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	if (bHasBlueprintActivate || bHasBlueprintActivateFromEvent)
	{
		Super::ActivateAbilityBody(Original, Handle, ActorInfo, ActivationInfo, TriggerEventData);
		return;
	}
	if (!TransitionMontage)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("SquadTransition [%s]: 未配置 TransitionMontage，能力以取消结束。"), *GetPathName());
		RequestAbilityEnd(Original, true, true);
		return;
	}

	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = UGGYGOAbilityTask_PlayMontageAndWaitForEvent::PlayMontageAndWaitForEvent(
		this, TEXT("SquadTransition"), TransitionMontage, FGameplayTagContainer());
	const TWeakObjectPtr<UGGYGOSquadTransitionAbility> WeakThis(this);
	const auto MakeEnd = [WeakThis, Original](bool bWasCancelled)
	{
		FGGYGOPlayMontageAndWaitForEventDelegate Callback;
		Callback.BindLambda([WeakThis, Original, bWasCancelled](FGameplayTag, FGameplayEventData)
		{
			if (UGGYGOSquadTransitionAbility* Ability = WeakThis.Get())
			{
				Ability->RequestAbilityEnd(Original, true, bWasCancelled);
			}
		});
		return Callback;
	};
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbacks Callbacks;
	Callbacks.OnCompleted = MakeEnd(false);
	Callbacks.OnInterrupted = MakeEnd(true);
	Callbacks.OnCancelled = MakeEnd(true);
	Callbacks.OnFailed.BindLambda([WeakThis, Original](const FGGYGOMontageTaskFailureFact&)
	{
		if (UGGYGOSquadTransitionAbility* Ability = WeakThis.Get())
		{
			Ability->RequestAbilityEnd(Original, true, true);
		}
	});
	if (!Task || !Task->RegisterNativeCallbacks(MoveTemp(Callbacks)).IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("SquadTransition [%s]: 无法创建 Montage 任务或登记回调，能力以取消结束。"), *GetPathName());
		RequestAbilityEnd(Original, true, true);
		return;
	}
	Task->ReadyForActivation();
}

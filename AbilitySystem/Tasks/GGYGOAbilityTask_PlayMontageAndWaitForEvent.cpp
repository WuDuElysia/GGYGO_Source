/**
 * @file GGYGOAbilityTask_PlayMontageAndWaitForEvent.cpp
 * @brief Montage 播放与事件等待实现
 */
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "GameFramework/Character.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAbilityTask_PlayMontageAndWaitForEvent)

UGGYGOAbilityTask_PlayMontageAndWaitForEvent::UGGYGOAbilityTask_PlayMontageAndWaitForEvent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	Rate = 1.0f;
	bStopWhenAbilityEnds = true;
	AnimRootMotionTranslationScale = 1.0f;
}

UGGYGOAbilityTask_PlayMontageAndWaitForEvent* UGGYGOAbilityTask_PlayMontageAndWaitForEvent::PlayMontageAndWaitForEvent(
	UGameplayAbility* OwningAbility,
	FName TaskInstanceName,
	UAnimMontage* MontageToPlay,
	FGameplayTagContainer EventTags,
	float Rate,
	FName StartSection,
	bool bStopWhenAbilityEnds,
	float AnimRootMotionTranslationScale)
{
	// 让 GAS 把速率变化算进预测。不调这个的话，客户端与服务器
	// 对"动画播到哪了"的判断会随速率偏离。
	UAbilitySystemGlobals::NonShipping_ApplyGlobalAbilityScaler_Rate(Rate);

	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = NewAbilityTask<UGGYGOAbilityTask_PlayMontageAndWaitForEvent>(OwningAbility, TaskInstanceName);
	Task->MontageToPlay = MontageToPlay;
	Task->EventTags = EventTags;
	Task->Rate = Rate;
	Task->StartSection = StartSection;
	Task->AnimRootMotionTranslationScale = AnimRootMotionTranslationScale;
	Task->bStopWhenAbilityEnds = bStopWhenAbilityEnds;

	return Task;
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::Activate()
{
	if (!Ability)
	{
		return;
	}

	UAbilitySystemComponent* ASC = AbilitySystemComponent.Get();
	if (!ASC)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayMontageAndWaitForEvent: 没有 ASC。"));
		return;
	}

	const FGameplayAbilityActorInfo* ActorInfo = Ability->GetCurrentActorInfo();
	UAnimInstance* AnimInstance = ActorInfo ? ActorInfo->GetAnimInstance() : nullptr;

	if (!AnimInstance)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("PlayMontageAndWaitForEvent: [%s] 的 Avatar 没有 AnimInstance。"), *Ability->GetName());

		if (ShouldBroadcastAbilityTaskDelegates())
		{
			OnCancelled.Broadcast(FGameplayTag(), FGameplayEventData());
		}
		return;
	}

	// 先订阅事件再播 Montage。
	//
	// 顺序不能反：Montage 的第一帧就可能带一个 AnimNotify（例如"起手瞬间开判定"），
	// 播完再订阅会漏掉它。
	EventHandle = ASC->AddGameplayEventTagContainerDelegate(
		EventTags,
		FGameplayEventTagMulticastDelegate::FDelegate::CreateUObject(this, &ThisClass::OnGameplayEvent));

	if (ASC->PlayMontage(Ability, Ability->GetCurrentActivationInfo(), MontageToPlay, Rate, StartSection) <= 0.0f)
	{
		// 播放失败（Montage 为空、Slot 配错、或被更高优先级的 Montage 拒绝）。
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("PlayMontageAndWaitForEvent: [%s] 播放 [%s] 失败。"),
			*Ability->GetName(), *GetNameSafe(MontageToPlay));

		if (ShouldBroadcastAbilityTaskDelegates())
		{
			OnCancelled.Broadcast(FGameplayTag(), FGameplayEventData());
		}
		return;
	}

	// 能力被取消时要一起收尾。取消可能来自组仲裁、死亡或玩家操作。
	CancelledHandle = Ability->OnGameplayAbilityCancelled.Add(
		FOnGameplayAbilityCancelled::FDelegate::CreateUObject(this, &ThisClass::OnAbilityCancelled));

	BlendingOutDelegate.BindUObject(this, &ThisClass::OnMontageBlendingOut);
	AnimInstance->Montage_SetBlendingOutDelegate(BlendingOutDelegate, MontageToPlay);

	MontageEndedDelegate.BindUObject(this, &ThisClass::OnMontageEnded);
	AnimInstance->Montage_SetEndDelegate(MontageEndedDelegate, MontageToPlay);

	// root motion 缩放只对本地控制端有意义：其余角色的位移来自复制。
	if (ACharacter* Character = Cast<ACharacter>(GetAvatarActor()))
	{
		if (Character->GetLocalRole() == ROLE_Authority
			|| (Character->GetLocalRole() == ROLE_AutonomousProxy && Ability->GetNetExecutionPolicy() == EGameplayAbilityNetExecutionPolicy::LocalPredicted))
		{
			Character->SetAnimRootMotionTranslationScale(AnimRootMotionTranslationScale);
		}
	}

	SetWaitingOnAvatar();
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ExternalCancel()
{
	if (ShouldBroadcastAbilityTaskDelegates())
	{
		OnCancelled.Broadcast(FGameplayTag(), FGameplayEventData());
	}

	Super::ExternalCancel();
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::OnDestroy(bool AbilityEnded)
{
	// 解绑事件监听是必需的。不解绑的话，后续动画里的同名事件会被这个
	// 已经过期的任务接到，表现为"上一招的判定在下一招里又开了一次"。
	if (Ability)
	{
		Ability->OnGameplayAbilityCancelled.Remove(CancelledHandle);

		if (AbilityEnded && bStopWhenAbilityEnds)
		{
			StopPlayingMontage();
		}
	}

	if (UAbilitySystemComponent* ASC = AbilitySystemComponent.Get())
	{
		ASC->RemoveGameplayEventTagContainerDelegate(EventTags, EventHandle);
	}

	Super::OnDestroy(AbilityEnded);
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::IsNotifyValid() const
{
	const FGameplayAbilityActorInfo* ActorInfo = Ability ? Ability->GetCurrentActorInfo() : nullptr;
	const UAnimInstance* AnimInstance = ActorInfo ? ActorInfo->GetAnimInstance() : nullptr;

	// 判断"当前正在播的是不是本任务播的那个 Montage"。
	// 不判断的话，本任务的 Montage 已被顶掉后仍会响应新 Montage 的结束事件。
	return AnimInstance && MontageToPlay && AnimInstance->Montage_IsPlaying(MontageToPlay);
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::StopPlayingMontage()
{
	const FGameplayAbilityActorInfo* ActorInfo = Ability ? Ability->GetCurrentActorInfo() : nullptr;
	UAnimInstance* AnimInstance = ActorInfo ? ActorInfo->GetAnimInstance() : nullptr;
	if (!AnimInstance)
	{
		return false;
	}

	UAbilitySystemComponent* ASC = AbilitySystemComponent.Get();
	if (!ASC || !Ability)
	{
		return false;
	}

	// 只在"当前动画确实是本能力播的"时才停。
	// 少了这个判断，能力结束时会把别的能力刚播上的动画一起停掉。
	const FAnimMontageInstance* MontageInstance = AnimInstance->GetActiveInstanceForMontage(MontageToPlay);
	if (ASC->GetAnimatingAbility() == Ability
		&& ASC->GetCurrentMontage() == MontageToPlay
		&& MontageInstance)
	{
		// 先清委托再停：Montage_Stop 会同步触发结束回调，
		// 不先清会在任务销毁过程中再走一遍广播。
		// 这两个 API 要非 const 左值引用，所以不能直接传临时对象。
		FOnMontageBlendingOutStarted EmptyBlendingOutDelegate;
		AnimInstance->Montage_SetBlendingOutDelegate(EmptyBlendingOutDelegate, MontageToPlay);

		FOnMontageEnded EmptyEndDelegate;
		AnimInstance->Montage_SetEndDelegate(EmptyEndDelegate, MontageToPlay);

		ASC->CurrentMontageStop();
		return true;
	}

	return false;
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::OnAbilityCancelled()
{
	if (StopPlayingMontage())
	{
		if (ShouldBroadcastAbilityTaskDelegates())
		{
			OnCancelled.Broadcast(FGameplayTag(), FGameplayEventData());
		}
	}
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::OnMontageBlendingOut(UAnimMontage* Montage, bool bInterrupted)
{
	// 只有本能力仍是动画的驱动者时才恢复 root motion 缩放，
	// 否则会覆盖掉接手动画的那个能力设的值。
	if (Ability && Ability->GetCurrentMontage() == MontageToPlay)
	{
		if (Montage == MontageToPlay)
		{
			if (UAbilitySystemComponent* ASC = AbilitySystemComponent.Get())
			{
				ASC->ClearAnimatingAbility(Ability);
			}

			if (ACharacter* Character = Cast<ACharacter>(GetAvatarActor()))
			{
				if (Character->GetLocalRole() == ROLE_Authority
					|| (Character->GetLocalRole() == ROLE_AutonomousProxy && Ability->GetNetExecutionPolicy() == EGameplayAbilityNetExecutionPolicy::LocalPredicted))
				{
					Character->SetAnimRootMotionTranslationScale(1.0f);
				}
			}
		}
	}

	if (!ShouldBroadcastAbilityTaskDelegates())
	{
		return;
	}

	// 被打断走 OnInterrupted，自然混出走 OnBlendOut。
	// 两者对能力的含义不同：前者要中断收尾，后者只是允许衔接下一个动作。
	if (bInterrupted)
	{
		OnInterrupted.Broadcast(FGameplayTag(), FGameplayEventData());
	}
	else
	{
		OnBlendOut.Broadcast(FGameplayTag(), FGameplayEventData());
	}
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::OnMontageEnded(UAnimMontage* Montage, bool bInterrupted)
{
	if (!bInterrupted && ShouldBroadcastAbilityTaskDelegates())
	{
		OnCompleted.Broadcast(FGameplayTag(), FGameplayEventData());
	}

	// 被打断的情况已经在 BlendingOut 里广播过了，这里不重复。
	EndTask();
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::OnGameplayEvent(FGameplayTag EventTag, const FGameplayEventData* Payload)
{
	if (!ShouldBroadcastAbilityTaskDelegates())
	{
		return;
	}

	// 拷一份负载再广播：原始指针指向 ASC 内部的临时对象，
	// 广播过程中若有人结束了任务，那块内存就失效了。
	FGameplayEventData TempData = *Payload;
	TempData.EventTag = EventTag;

	EventReceived.Broadcast(EventTag, TempData);
}

FString UGGYGOAbilityTask_PlayMontageAndWaitForEvent::GetDebugString() const
{
	const UAnimMontage* PlayingMontage = nullptr;

	if (Ability)
	{
		if (const FGameplayAbilityActorInfo* ActorInfo = Ability->GetCurrentActorInfo())
		{
			if (const UAnimInstance* AnimInstance = ActorInfo->GetAnimInstance())
			{
				PlayingMontage = AnimInstance->Montage_IsActive(MontageToPlay)
					? MontageToPlay.Get()
					: AnimInstance->GetCurrentActiveMontage();
			}
		}
	}

	return FString::Printf(TEXT("PlayMontageAndWaitForEvent. MontageToPlay: %s (playing %s)"),
		*GetNameSafe(MontageToPlay), *GetNameSafe(PlayingMontage));
}

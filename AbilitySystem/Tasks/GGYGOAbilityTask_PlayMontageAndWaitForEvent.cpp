/**
 * @file GGYGOAbilityTask_PlayMontageAndWaitForEvent.cpp
 * @brief Montage 播放与事件等待实现
 */
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/Tasks/GGYGORootMotionScaleLease.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/Runtime/GGYGOMontageGuardAnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAbilityTask_PlayMontageAndWaitForEvent)

struct UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FInFlightMontagePlayCleanup
{
	// One original call's cleanup obligation, shared with its stack until the ASC returns.
	bool bStopRequested = false;
};

struct UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbackRegistration
{
	explicit FNativeCallbackRegistration(FNativeCallbacks&& InCallbacks)
		: Handle(FDelegateHandle::GenerateNewHandle), Callbacks(MoveTemp(InCallbacks)) {}

	const FDelegateHandle Handle;
	const FNativeCallbacks Callbacks;
};

namespace
{
	bool StopOriginalMontageInstance(const FGGYGOMontagePlayGuardIdentity& Identity,
		const FGGYGOAbilityMontagePlaybackHandle& Playback,
		const TWeakObjectPtr<UAbilitySystemComponent>& OriginalASC,
		const TWeakObjectPtr<UAnimMontage>& OriginalMontage,
		const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent>& OriginalTask)
	{
		UGGYGOMontageGuardAnimInstance* Guard = Cast<UGGYGOMontageGuardAnimInstance>(
			Identity.OriginalAnimInstance.Get());
		UAnimMontage* Montage = OriginalMontage.Get();
		if (!Guard || !Guard->IsMontagePlayGuardIdentityCurrent(Identity)
			|| Identity.CreatedInstanceId == INDEX_NONE || !Montage)
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("MontageTask [%s] 无法清理原实例：ASC [%s]，Montage [%s]，Anim [%s]，Generation=%llu Call=%llu Instance=%d；原 Guard 身份无效，未猜测播放实例。"),
				*GetNameSafe(OriginalTask.Get()), *GetNameSafe(OriginalASC.Get()), *GetNameSafe(Montage),
				*GetNameSafe(Identity.OriginalAnimInstance.Get()), Identity.LifecycleGeneration,
				Identity.CallId, Identity.CreatedInstanceId);
			return false;
		}
		FAnimMontageInstance* Instance = Guard->GetMontageInstanceForID(Identity.CreatedInstanceId);
		if (!Instance || Instance->Montage != Montage || !Instance->IsActive())
		{
			return false; // An already released exact local resource needs no substitute cleanup.
		}
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get();
		if ((Instance->OnMontageEnded.IsBound() && (!Task || Instance->OnMontageEnded.GetUObject() != Task))
			|| (Instance->OnMontageBlendingOutStarted.IsBound()
				&& (!Task || Instance->OnMontageBlendingOutStarted.GetUObject() != Task)))
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("MontageTask [%s] 拒绝清理原实例：Montage [%s]，Call=%llu Instance=%d；实例委托已有其它所有者。"),
				*GetNameSafe(Task), *GetNameSafe(Montage), Identity.CallId, Identity.CreatedInstanceId);
			return false;
		}
		if (Task && Instance->OnMontageEnded.GetUObject() == Task) { Instance->OnMontageEnded.Unbind(); }
		if (Task && Instance->OnMontageBlendingOutStarted.GetUObject() == Task)
		{
			Instance->OnMontageBlendingOutStarted.Unbind();
		}
		if (Task && Instance->OnMontageSectionChanged.GetUObject() == Task)
		{
			Instance->OnMontageSectionChanged.Unbind();
		}

		UGGYGOAbilitySystemComponent* ASC = Cast<UGGYGOAbilitySystemComponent>(OriginalASC.Get());
		const FAnimMontageInstance* ActiveInstance = Guard->GetActiveInstanceForMontage(Montage);
		if (ASC && Playback.HasPlayback()
			&& ASC->CheckMontagePlaybackOwnership(Playback).Outcome == EGGYGOAbilityMontagePlaybackOutcome::Succeeded
			&& ActiveInstance == Instance)
		{
			// Proven original ownership plus exact active ID allows the existing GAS replication path.
			ASC->CurrentMontageStop();
			return true;
		}

		// Local resource release only: no ASC/GA clear and no lookup through a replacement Avatar.
		FMontageBlendSettings BlendOutSettings;
		BlendOutSettings.Blend = Montage->BlendOut;
		BlendOutSettings.BlendMode = Montage->BlendModeOut;
		BlendOutSettings.BlendProfile = Montage->BlendProfileOut;
		Instance->Stop(BlendOutSettings);
		return true;
	}
}

UGGYGOAbilityTask_PlayMontageAndWaitForEvent::UGGYGOAbilityTask_PlayMontageAndWaitForEvent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	Rate = 1.0f;
	bStopWhenAbilityEnds = true;
	AnimRootMotionTranslationScale = 1.0f;
}

FDelegateHandle UGGYGOAbilityTask_PlayMontageAndWaitForEvent::RegisterNativeCallbacks(FNativeCallbacks Callbacks)
{
	check(IsInGameThread());
	const TCHAR* Reason = nullptr;
	if (HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) || bEndingTask || bCancellationRequested
		|| GetState() != EGameplayTaskState::AwaitingActivation)
	{
		Reason = TEXT("Task is not in its original pre-Ready AwaitingActivation state");
	}
	else if (NativeCallbackRegistration.IsValid()) { Reason = TEXT("a native callback package is already registered"); }
	else if (!Callbacks.OnCompleted.IsBound() && !Callbacks.OnBlendOut.IsBound()
		&& !Callbacks.OnInterrupted.IsBound() && !Callbacks.OnCancelled.IsBound()
		&& !Callbacks.EventReceived.IsBound() && !Callbacks.SectionReceived.IsBound())
	{
		Reason = TEXT("the native callback package has no bound callbacks");
	}
	if (Reason)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("MontageTask [%s] native registration rejected: Ability [%s], Montage [%s]; %s."),
			*GetPathName(), *GetNameSafe(Ability), *GetNameSafe(MontageToPlay), Reason);
		return {};
	}

	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(this);
	const FString OriginalPath = GetPathName();
	TSharedPtr<FNativeCallbackRegistration> Registration = MakeShared<FNativeCallbackRegistration>(MoveTemp(Callbacks));
	// Delegate construction may execute capture code. Recheck before installing this exact package.
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get();
	if (!Task || Task->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) || Task->bEndingTask
		|| Task->bCancellationRequested || Task->GetState() != EGameplayTaskState::AwaitingActivation
		|| Task->NativeCallbackRegistration.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("MontageTask [%s] native registration rejected after callback construction: original pre-Ready scope changed."),
			*OriginalPath);
		return {};
	}
	const FDelegateHandle Handle = Registration->Handle;
	Task->NativeCallbackRegistration = MoveTemp(Registration);
	return Handle; // No task writes during subsequent destruction of argument captures.
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::UnregisterNativeCallbacks(FDelegateHandle Registration)
{
	check(IsInGameThread());
	if (!Registration.IsValid() || !NativeCallbackRegistration.IsValid()
		|| NativeCallbackRegistration->Handle != Registration) { return false; }
	TSharedPtr<FNativeCallbackRegistration> Detached = MoveTemp(NativeCallbackRegistration);
	Detached.Reset(); // Capture destruction may register a successor; no task writes follow.
	return true;
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::CanDispatchOriginalCallback(
	ENativeCallback Kind, const FGGYGOMontagePlayGuardIdentity& Original) const
{
	if (HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) || bEndingTask
		|| GetState() == EGameplayTaskState::Finished || !ShouldBroadcastAbilityTaskDelegates()) { return false; }
	if (Kind == ENativeCallback::Cancelled)
	{
		return bCancellationRequested; // Startup failure need not have installed ActorInfo or a Guard.
	}
	if (!MatchesOriginalMontageCallback(Original)) { return false; }
	if (Kind == ENativeCallback::EventReceived) { return IsNotifyValid(); }
	if (Kind != ENativeCallback::Completed && Kind != ENativeCallback::BlendOut
		&& Kind != ENativeCallback::Interrupted) { return false; }
	return CanDispatchOriginalInstanceFact(Original);
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::CanDispatchOriginalInstanceFact(
	const FGGYGOMontagePlayGuardIdentity& Original) const
{
	if (HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) || bEndingTask
		|| GetState() == EGameplayTaskState::Finished || !ShouldBroadcastAbilityTaskDelegates()
		|| !MatchesOriginalMontageCallback(Original)) { return false; }
	const UGGYGOMontageGuardAnimInstance* Guard = Cast<UGGYGOMontageGuardAnimInstance>(ActivatedAnimInstance.Get());
	// The engine dispatches queued section facts after blend-out and before Ended. These facts,
	// like natural Ended, do not require current ASC playback ownership or a surviving instance.
	return Guard && Guard->IsMontagePlayGuardIdentityCurrent(Original) && IsActivatedActorInfoCurrent();
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::DispatchOriginalCallback(
	ENativeCallback Kind, FGameplayTag EventTag, FGameplayEventData EventData)
{
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(this);
	const TWeakObjectPtr<UGameplayAbility> OriginalAbility(Ability);
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalASC(AbilitySystemComponent.Get());
	const FGGYGOMontagePlayGuardIdentity OriginalGuard = OriginalGuardIdentity;
	const auto Recheck = [&]() -> UGGYGOAbilityTask_PlayMontageAndWaitForEvent*
	{
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get();
		return Task && OriginalAbility.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGameplayAbility>(Task->Ability))
			&& OriginalASC.HasSameIndexAndSerialNumber(TWeakObjectPtr<UAbilitySystemComponent>(Task->AbilitySystemComponent.Get()))
			&& Task->CanDispatchOriginalCallback(Kind, OriginalGuard) ? Task : nullptr;
	};
	if (!Recheck()) { return; }

	// This immutable registration is the callback snapshot. Copying its shared reference does not
	// copy user functors; unregister/OnDestroy may detach the member while this call owns its copy.
	TSharedPtr<FNativeCallbackRegistration> NativeSnapshot = NativeCallbackRegistration;
	if (NativeSnapshot.IsValid())
	{
		const FGGYGOPlayMontageAndWaitForEventDelegate* Callback = nullptr;
		switch (Kind)
		{
		case ENativeCallback::Completed: Callback = &NativeSnapshot->Callbacks.OnCompleted; break;
		case ENativeCallback::BlendOut: Callback = &NativeSnapshot->Callbacks.OnBlendOut; break;
		case ENativeCallback::Interrupted: Callback = &NativeSnapshot->Callbacks.OnInterrupted; break;
		case ENativeCallback::Cancelled: Callback = &NativeSnapshot->Callbacks.OnCancelled; break;
		case ENativeCallback::EventReceived: Callback = &NativeSnapshot->Callbacks.EventReceived; break;
		}
		if (Callback) { Callback->ExecuteIfBound(EventTag, EventData); }
	}
	NativeSnapshot.Reset(); // Capture destructors are also external code; recheck only after they return.
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = Recheck();
	if (!Task) { return; }
	switch (Kind)
	{
	case ENativeCallback::Completed: Task->OnCompleted.Broadcast(EventTag, EventData); break;
	case ENativeCallback::BlendOut: Task->OnBlendOut.Broadcast(EventTag, EventData); break;
	case ENativeCallback::Interrupted: Task->OnInterrupted.Broadcast(EventTag, EventData); break;
	case ENativeCallback::Cancelled: Task->OnCancelled.Broadcast(EventTag, EventData); break;
	case ENativeCallback::EventReceived: Task->EventReceived.Broadcast(EventTag, EventData); break;
	}
	// No member access after BP. Callers with an old cleanup tail reacquire their weak original task.
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::DispatchOriginalSectionFact(
	const FGGYGOMontageSectionFact& Fact, const FGGYGOMontagePlayGuardIdentity& Original)
{
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(this);
	const TWeakObjectPtr<UGameplayAbility> OriginalAbility(Ability);
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalASC(AbilitySystemComponent.Get());
	const auto Recheck = [&]() -> UGGYGOAbilityTask_PlayMontageAndWaitForEvent*
	{
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get();
		return Task && OriginalAbility.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGameplayAbility>(Task->Ability))
			&& OriginalASC.HasSameIndexAndSerialNumber(TWeakObjectPtr<UAbilitySystemComponent>(Task->AbilitySystemComponent.Get()))
			&& Fact.Montage == Task->MontageToPlay && Fact.MontageInstanceId == Original.CreatedInstanceId
			&& Task->CanDispatchOriginalInstanceFact(Original) ? Task : nullptr;
	};
	if (!Recheck()) { return; }
	TSharedPtr<FNativeCallbackRegistration> NativeSnapshot = NativeCallbackRegistration;
	if (NativeSnapshot.IsValid()) { NativeSnapshot->Callbacks.SectionReceived.ExecuteIfBound(Fact); }
	NativeSnapshot.Reset(); // Capture destruction is external code too.
	if (UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = Recheck())
	{
		Task->SectionReceived.Broadcast(Fact);
	}
	// No member access after BP; the original task may have ended or launched a successor.
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::TryGetOriginalSectionSnapshot(
	FGGYGOMontageSectionSnapshot& OutSnapshot) const
{
	check(IsInGameThread());
	OutSnapshot = {};
	if (!CanDispatchOriginalInstanceFact(OriginalGuardIdentity)) { return false; }
	const FAnimMontageInstance* Instance = GetTaskMontageInstance();
	if (!Instance) { return false; } // The exact instance may be blending out after ASC ownership retired.
	const float Position = Instance->GetPosition();
	const float InstanceRate = Instance->GetPlayRate();
	if (!FMath::IsFinite(Position) || !FMath::IsFinite(InstanceRate)) { return false; }
	OutSnapshot.Montage = MontageToPlay;
	OutSnapshot.MontageInstanceId = Instance->GetInstanceID();
	OutSnapshot.SectionName = Instance->GetCurrentSection();
	OutSnapshot.PositionSeconds = Position;
	OutSnapshot.InstancePlayRate = InstanceRate;
	return true;
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolvePlayRate(
	const UAnimMontage* Montage,
	float RequestedRate,
	float& OutTaskPlayRate,
	float& OutEffectivePlayRate)
{
	OutTaskPlayRate = 0.0f;
	OutEffectivePlayRate = 0.0f;
	if (!Montage || !FMath::IsFinite(RequestedRate) || RequestedRate <= 0.0f
		|| !FMath::IsFinite(Montage->RateScale) || Montage->RateScale <= 0.0f)
	{
		return false;
	}

	float TaskPlayRate = RequestedRate;
	UAbilitySystemGlobals::NonShipping_ApplyGlobalAbilityScaler_Rate(TaskPlayRate);
	const float EffectivePlayRate = TaskPlayRate * Montage->RateScale;
	if (!FMath::IsFinite(TaskPlayRate) || TaskPlayRate <= 0.0f
		|| !FMath::IsFinite(EffectivePlayRate) || EffectivePlayRate <= 0.0f)
	{
		return false;
	}

	OutTaskPlayRate = TaskPlayRate;
	OutEffectivePlayRate = EffectivePlayRate;
	return true;
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
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = NewAbilityTask<UGGYGOAbilityTask_PlayMontageAndWaitForEvent>(OwningAbility, TaskInstanceName);
	Task->MontageToPlay = MontageToPlay;
	Task->EventTags = EventTags;
	Task->bHasValidConfiguration = FMath::IsFinite(AnimRootMotionTranslationScale)
		&& ResolvePlayRate(MontageToPlay, Rate, Task->Rate, Task->EffectivePlayRate);
	Task->StartSection = StartSection;
	Task->AnimRootMotionTranslationScale = AnimRootMotionTranslationScale;
	Task->bStopWhenAbilityEnds = bStopWhenAbilityEnds;

	return Task;
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::Activate()
{
	if (bEndingTask || bCancellationRequested || GetState() == EGameplayTaskState::Finished) { return; }
	if (InFlightMontagePlayCleanup.IsValid() || OriginalGuardIdentity.CallId != 0)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("MontageTask [%s] 拒绝重复 Activate：原播放任务不可复用。"), *GetNameSafe(this));
		return;
	}
	if (!Ability || !Ability->IsActive()) { EndTask(); return; }
	if (!bHasValidConfiguration)
	{
		FailAndEndTask(TEXT("Montage、播放速率或 RootMotion 缩放配置无效"));
		return;
	}
	UGGYGOAbilitySystemComponent* ASC = Cast<UGGYGOAbilitySystemComponent>(AbilitySystemComponent.Get());
	if (!ASC)
	{
		FailAndEndTask(TEXT("缺少必需的项目 ASC"));
		return;
	}
	const TSharedPtr<FGameplayAbilityActorInfo> OriginalActorInfo = ASC->AbilityActorInfo;
	const FGameplayAbilityActorInfo* ActorInfo = Ability->GetCurrentActorInfo();
	AActor* AvatarActor = ActorInfo ? ActorInfo->AvatarActor.Get() : nullptr;
	UAnimInstance* AnimInstance = ActorInfo ? ActorInfo->GetAnimInstance() : nullptr;
	if (!ActorInfo || OriginalActorInfo.Get() != ActorInfo || !AvatarActor || !AnimInstance
		|| !ActorInfo->OwnerActor.IsValid() || !ActorInfo->SkeletalMeshComponent.IsValid()
		|| ActorInfo->AbilitySystemComponent.Get() != ASC)
	{
		FailAndEndTask(TEXT("原 ActorInfo、Owner、Avatar、Mesh 或 AnimInstance 无效"));
		return;
	}

	ActivatedActorInfo = OriginalActorInfo;
	ActivatedAbility = Ability;
	ActivatedOwnerActor = ActorInfo->OwnerActor;
	ActivatedAvatarActor = AvatarActor;
	ActivatedMesh = ActorInfo->SkeletalMeshComponent;
	ActivatedCharacter = Cast<ACharacter>(AvatarActor);
	ActivatedAnimInstance = AnimInstance;
	ActivatedASC = ASC;
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(this);
	const TWeakObjectPtr<UGameplayAbility> OriginalAbility(Ability);
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalASC(ASC);
	const TWeakObjectPtr<UAnimMontage> OriginalMontage(MontageToPlay);
	const FGameplayAbilityActivationInfo OriginalActivationInfo = Ability->GetCurrentActivationInfo();
	const bool bStopOnOwnerEnd = bStopWhenAbilityEnds;
	const TSharedRef<FInFlightMontagePlayCleanup> Cleanup = MakeShared<FInFlightMontagePlayCleanup>();
	InFlightMontagePlayCleanup = Cleanup;

	// Both subscriptions belong to this original Task, including cancellation during native play.
	CancelledHandle = Ability->OnGameplayAbilityCancelled.Add(
		FOnGameplayAbilityCancelled::FDelegate::CreateUObject(this, &ThisClass::OnAbilityCancelled));
	EventHandle = ASC->AddGameplayEventTagContainerDelegate(EventTags,
		FGameplayEventTagMulticastDelegate::FDelegate::CreateUObject(this, &ThisClass::OnGameplayEvent));

	const FGGYGOAbilityMontagePlaybackResult Result = ASC->TryPlayMontageWithOwnership(
		OriginalAbility.Get(), OriginalActivationInfo, OriginalMontage.Get(), Rate, StartSection,
		StartTimeSeconds, [OriginalTask, OriginalAbility, OriginalASC]()
		{
			const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get();
			// Pure Task liveness only. The ASC owns native activation/playback authentication.
			return Task && !Task->bEndingTask && !Task->bCancellationRequested
				&& Task->GetState() != EGameplayTaskState::Finished
				&& Task->Ability == OriginalAbility.Get() && OriginalAbility.IsValid()
				&& Task->AbilitySystemComponent.Get() == OriginalASC.Get()
				&& Task->IsActivatedActorInfoCurrent();
		});

	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get();
	const bool bTaskOpen = Task && !Task->bEndingTask && !Task->bCancellationRequested
		&& Task->GetState() != EGameplayTaskState::Finished;
	const bool bOriginalAbilityActive = OriginalAbility.IsValid() && OriginalAbility->IsActive();
	const bool bTaskCurrent = bTaskOpen && bOriginalAbilityActive
		&& Task->Ability == OriginalAbility.Get() && Task->IsActivatedActorInfoCurrent();
	if (!bTaskCurrent)
	{
		const bool bLostLiveContext = bTaskOpen && bOriginalAbilityActive;
		// OnDestroy may precede the return which first exposes this exact created instance.
		if (Cleanup->bStopRequested || (bStopOnOwnerEnd && !bOriginalAbilityActive) || bLostLiveContext)
		{
			if (Result.Guard.NativeStage != EGGYGOMontagePlayGuardNativeStage::NotEntered)
			{
				StopOriginalMontageInstance(Result.Guard.Identity, Result.Playback,
					OriginalASC, OriginalMontage, OriginalTask);
			}
		}
		Task = OriginalTask.Get();
		if (Task && !Task->bEndingTask)
		{
			if (bLostLiveContext && !Task->bCancellationRequested)
			{
				Task->FailAndEndTask(TEXT("外调返回时原 Task、Ability 或 ActorInfo 关联已失效"), &Result);
			}
			else { Task->EndTask(); }
		}
		return; // No installation or writes through a Task ended by the external call.
	}
	Task->InFlightMontagePlayCleanup.Reset();
	if (Result.Outcome != EGGYGOAbilityMontagePlaybackOutcome::Succeeded || !Result.Playback.HasPlayback()
		|| Result.Guard.Outcome != EGGYGOMontagePlayGuardOutcome::Accepted
		|| Result.Guard.Identity.CreatedInstanceId == INDEX_NONE
		|| !FMath::IsFinite(Result.Duration) || Result.Duration <= 0.0f)
	{
		if (Result.Guard.NativeStage != EGGYGOMontagePlayGuardNativeStage::NotEntered)
		{
			StopOriginalMontageInstance(Result.Guard.Identity, Result.Playback,
				OriginalASC, OriginalMontage, OriginalTask);
		}
		Task = OriginalTask.Get();
		if (Task && !Task->bEndingTask) { Task->FailAndEndTask(TEXT("ASC 未签发成功的原播放资源"), &Result); }
		return;
	}

	Task->OriginalPlayback = Result.Playback;
	Task->OriginalGuardIdentity = Result.Guard.Identity;
	Task->MontageInstanceId = Result.Guard.Identity.CreatedInstanceId;
	FAnimMontageInstance* Instance = Task->GetTaskMontageInstance();
	if (!Instance || !Instance->IsActive() || !Instance->IsPlaying())
	{
		Task->StopPlayingMontage();
		Task = OriginalTask.Get();
		if (Task && !Task->bEndingTask) { Task->FailAndEndTask(TEXT("原 Guard 实例已失效，无法安装任务委托"), &Result); }
		return;
	}
	// Bind the result's exact instance, never the current instance found by asset.
	if (Instance->OnMontageSectionChanged.IsBound() && Instance->OnMontageSectionChanged.GetUObject() != Task)
	{
		Task->FailAndEndTask(TEXT("原实例 Section 委托已由其它对象持有，未覆盖其注册"));
		return;
	}
	Task->BlendingOutDelegate.BindUObject(Task, &ThisClass::OnMontageBlendingOutForInstance, Result.Guard.Identity);
	Instance->OnMontageBlendingOutStarted = Task->BlendingOutDelegate;
	Task->MontageEndedDelegate.BindUObject(Task, &ThisClass::OnMontageEndedForInstance, Result.Guard.Identity);
	Instance->OnMontageEnded = Task->MontageEndedDelegate;
	Task->MontageSectionChangedDelegate.BindUObject(Task, &ThisClass::OnMontageSectionChangedForInstance, Result.Guard.Identity);
	Instance->OnMontageSectionChanged = Task->MontageSectionChangedDelegate;

	if (ACharacter* Character = Task->ActivatedCharacter.Get())
	{
		if (Character->GetLocalRole() == ROLE_Authority
			|| (Character->GetLocalRole() == ROLE_AutonomousProxy
				&& OriginalAbility->GetNetExecutionPolicy() == EGameplayAbilityNetExecutionPolicy::LocalPredicted))
		{
			Task->RootMotionScaleLeaseToken = FGGYGORootMotionScaleLease::Acquire(
				Character, Task, Task->AnimRootMotionTranslationScale);
		}
	}
	Task->SetWaitingOnAvatar(); // May notify the ability; no member writes follow.
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ExternalCancel()
{
	if (bEndingTask || bCancellationRequested) { return; }
	bCancellationRequested = true;
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(this);
	RequestInFlightMontageStop();
	ReleaseRootMotionScaleLease();
	StopPlayingMontage();
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get();
	if (!Task || Task->bEndingTask || Task->GetState() == EGameplayTaskState::Finished) { return; }
	Task->DispatchOriginalCallback(ENativeCallback::Cancelled, FGameplayTag(), FGameplayEventData());
	Task = OriginalTask.Get();
	if (Task && !Task->bEndingTask && Task->GetState() != EGameplayTaskState::Finished) { Task->Super::ExternalCancel(); }
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::OnDestroy(bool AbilityEnded)
{
	if (bEndingTask) { return; }
	bEndingTask = true;
	// Close the subscription before any cleanup. Its captures stay stack-owned until all old
	// cleanup and Super have returned, so their destructors cannot interrupt a member-write tail.
	TSharedPtr<FNativeCallbackRegistration> DetachedNativeCallbacks = MoveTemp(NativeCallbackRegistration);
	if (AbilityEnded && bStopWhenAbilityEnds) { RequestInFlightMontageStop(); }
	InFlightMontagePlayCleanup.Reset(); // The original native call's stack still holds its obligation.
	if (UGameplayAbility* OriginalAbility = ActivatedAbility.Get())
	{
		OriginalAbility->OnGameplayAbilityCancelled.Remove(CancelledHandle);
	}
	CancelledHandle.Reset();
	if (UAbilitySystemComponent* ASC = ActivatedASC.Get())
	{
		if (EventHandle.IsValid()) { ASC->RemoveGameplayEventTagContainerDelegate(EventTags, EventHandle); }
	}
	EventHandle.Reset();
	UnbindTaskMontageDelegates(GetTaskMontageInstance());
	BlendingOutDelegate.Unbind();
	MontageEndedDelegate.Unbind();
	MontageSectionChangedDelegate.Unbind();
	ReleaseRootMotionScaleLease();
	if (AbilityEnded && bStopWhenAbilityEnds) { StopPlayingMontage(); }
	Super::OnDestroy(AbilityEnded);
	DetachedNativeCallbacks.Reset(); // External capture destruction; no task access follows.
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::IsNotifyValid() const
{
	const UGGYGOAbilitySystemComponent* ASC = Cast<UGGYGOAbilitySystemComponent>(ActivatedASC.Get());
	const FAnimMontageInstance* Instance = GetTaskMontageInstance();
	return !bEndingTask && !bCancellationRequested && !bBlendingOut && IsActivatedActorInfoCurrent()
		&& ASC && OriginalPlayback.HasPlayback() && Instance && Instance->IsActive() && Instance->IsPlaying()
		&& ASC->CheckMontagePlaybackOwnership(OriginalPlayback).Outcome == EGGYGOAbilityMontagePlaybackOutcome::Succeeded;
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::IsActivatedActorInfoCurrent() const
{
	const TSharedPtr<FGameplayAbilityActorInfo> OriginalInfo = ActivatedActorInfo.Pin();
	const UAbilitySystemComponent* ASC = ActivatedASC.Get();
	const UGameplayAbility* OriginalAbility = ActivatedAbility.Get();
	const UAnimInstance* AnimInstance = ActivatedAnimInstance.Get();
	const AActor* OwnerActor = ActivatedOwnerActor.Get();
	const AActor* AvatarActor = ActivatedAvatarActor.Get();
	const USkeletalMeshComponent* Mesh = ActivatedMesh.Get();
	const FGameplayAbilityActorInfo* CurrentInfo = ASC && ASC->AbilityActorInfo.IsValid()
		? ASC->AbilityActorInfo.Get() : nullptr;
	return OriginalInfo.IsValid() && CurrentInfo == OriginalInfo.Get()
		&& OriginalAbility && Ability == OriginalAbility && AbilitySystemComponent.Get() == ASC
		&& OriginalAbility->GetCurrentActorInfo() == CurrentInfo
		&& OwnerActor && AvatarActor && Mesh && AnimInstance
		&& CurrentInfo->AbilitySystemComponent.Get() == ASC && CurrentInfo->OwnerActor.Get() == OwnerActor
		&& CurrentInfo->AvatarActor.Get() == AvatarActor && CurrentInfo->SkeletalMeshComponent.Get() == Mesh
		&& CurrentInfo->GetAnimInstance() == AnimInstance;
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::MatchesOriginalMontageCallback(
	const FGGYGOMontagePlayGuardIdentity& Original) const
{
	// The engine queues a copy of the exact instance delegate. The instance may already be gone.
	return !bEndingTask && !bCancellationRequested && Original.CallId != 0 && Original.CreatedInstanceId != INDEX_NONE
		&& Original.CallId == OriginalGuardIdentity.CallId
		&& Original.LifecycleGeneration == OriginalGuardIdentity.LifecycleGeneration
		&& Original.CreatedInstanceId == MontageInstanceId
		&& Original.CreatedInstanceId == OriginalGuardIdentity.CreatedInstanceId
		&& Original.OriginalAnimInstance.HasSameIndexAndSerialNumber(OriginalGuardIdentity.OriginalAnimInstance);
}

FAnimMontageInstance* UGGYGOAbilityTask_PlayMontageAndWaitForEvent::GetTaskMontageInstance() const
{
	UGGYGOMontageGuardAnimInstance* Guard = Cast<UGGYGOMontageGuardAnimInstance>(ActivatedAnimInstance.Get());
	if (!Guard || MontageInstanceId == INDEX_NONE || MontageInstanceId != OriginalGuardIdentity.CreatedInstanceId
		|| !Guard->IsMontagePlayGuardIdentityCurrent(OriginalGuardIdentity))
	{
		return nullptr;
	}
	FAnimMontageInstance* Instance = Guard->GetMontageInstanceForID(MontageInstanceId);
	return Instance && Instance->Montage == MontageToPlay ? Instance : nullptr;
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::UnbindTaskMontageDelegates(FAnimMontageInstance* Instance)
{
	if (!Instance) { return; }
	if (Instance->OnMontageBlendingOutStarted.GetUObject() == this) { Instance->OnMontageBlendingOutStarted.Unbind(); }
	if (Instance->OnMontageEnded.GetUObject() == this) { Instance->OnMontageEnded.Unbind(); }
	if (Instance->OnMontageSectionChanged.GetUObject() == this) { Instance->OnMontageSectionChanged.Unbind(); }
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::RequestInFlightMontageStop()
{
	if (InFlightMontagePlayCleanup.IsValid()) { InFlightMontagePlayCleanup->bStopRequested = true; }
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FailAndEndTask(
	const TCHAR* Reason, const FGGYGOAbilityMontagePlaybackResult* Result)
{
	UE_LOG(LogGGYGOAbilitySystem, Error,
		TEXT("MontageTask [%s] 启动失败：Ability [%s]，ASC [%s]，Avatar [%s]，Montage [%s]；%s；Outcome=%d Reason=%d Guard=%d Stage=%d Call=%llu Instance=%d。"),
		*GetNameSafe(this), *GetNameSafe(Ability), *GetNameSafe(AbilitySystemComponent.Get()),
		*GetNameSafe(ActivatedAvatarActor.Get()), *GetNameSafe(MontageToPlay), Reason,
		Result ? int32(Result->Outcome) : -1, Result ? int32(Result->Reason) : -1,
		Result ? int32(Result->Guard.Outcome) : -1, Result ? int32(Result->Guard.NativeStage) : -1,
		Result ? Result->Guard.Identity.CallId : uint64(0), Result ? Result->Guard.Identity.CreatedInstanceId : INDEX_NONE);
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(this);
	if (!bEndingTask && !bCancellationRequested && ShouldBroadcastAbilityTaskDelegates())
	{
		bCancellationRequested = true;
		DispatchOriginalCallback(ENativeCallback::Cancelled, FGameplayTag(), FGameplayEventData());
	}
	if (UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get())
	{
		if (!Task->bEndingTask && Task->GetState() != EGameplayTaskState::Finished) { Task->EndTask(); }
	}
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ReleaseRootMotionScaleLease()
{
	const uint64 OriginalToken = RootMotionScaleLeaseToken;
	RootMotionScaleLeaseToken = 0;
	if (OriginalToken != 0) { FGGYGORootMotionScaleLease::Release(ActivatedCharacter, this, OriginalToken); }
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::StopPlayingMontage()
{
	if (MontageInstanceId == INDEX_NONE) { return false; } // In-flight play returns its exact ID later.
	return StopOriginalMontageInstance(OriginalGuardIdentity, OriginalPlayback, ActivatedASC,
		TWeakObjectPtr<UAnimMontage>(MontageToPlay.Get()), TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent>(this));
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::OnAbilityCancelled()
{
	if (bEndingTask || bCancellationRequested) { return; }
	bCancellationRequested = true;
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(this);
	RequestInFlightMontageStop();
	ReleaseRootMotionScaleLease();
	StopPlayingMontage();
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get();
	if (Task && !Task->bEndingTask && Task->GetState() != EGameplayTaskState::Finished)
	{
		Task->DispatchOriginalCallback(ENativeCallback::Cancelled, FGameplayTag(), FGameplayEventData());
	}
	Task = OriginalTask.Get();
	if (Task && !Task->bEndingTask && Task->GetState() != EGameplayTaskState::Finished) { Task->EndTask(); }
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::OnMontageBlendingOutForInstance(
	UAnimMontage* Montage, bool bInterrupted, FGGYGOMontagePlayGuardIdentity Original)
{
	if (MatchesOriginalMontageCallback(Original)) { OnMontageBlendingOut(Montage, bInterrupted); }
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::OnMontageBlendingOut(UAnimMontage* Montage, bool bInterrupted)
{
	if (bEndingTask || bCancellationRequested || bBlendingOut || Montage != MontageToPlay) { return; }
	bBlendingOut = true;
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(this);
	const FGGYGOAbilityMontagePlaybackHandle Playback = OriginalPlayback;
	const FGGYGOMontagePlayGuardIdentity GuardIdentity = OriginalGuardIdentity;
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalASC = ActivatedASC;
	const TWeakObjectPtr<UAnimMontage> OriginalMontage(MontageToPlay);
	ReleaseRootMotionScaleLease();
	if (UGGYGOAbilitySystemComponent* ASC = Cast<UGGYGOAbilitySystemComponent>(OriginalASC.Get()))
	{
		if (Playback.HasPlayback())
		{
			const FGGYGOAbilityMontageClearResult Cleared = ASC->TryClearMontageAnimatingAbility(Playback);
			if (Cleared.Outcome == EGGYGOAbilityMontagePlaybackOutcome::Failed
				|| Cleared.Outcome == EGGYGOAbilityMontagePlaybackOutcome::Rejected)
			{
				UE_LOG(LogGGYGOAbilitySystem, Warning,
					TEXT("MontageTask [%s] 原混出归属清除失败：ASC [%s]，Montage [%s]，Call=%llu Instance=%d，Outcome=%d Reason=%d。"),
					*GetNameSafe(OriginalTask.Get()), *GetNameSafe(OriginalASC.Get()), *GetNameSafe(OriginalMontage.Get()),
					GuardIdentity.CallId, GuardIdentity.CreatedInstanceId, int32(Cleared.Outcome), int32(Cleared.Reason));
			}
		}
	}
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get();
	if (!Task || !Task->MatchesOriginalMontageCallback(GuardIdentity)) { return; }
	// These are original instance facts. They do not grant authority over the current GA resource.
	Task->DispatchOriginalCallback(bInterrupted ? ENativeCallback::Interrupted : ENativeCallback::BlendOut,
		FGameplayTag(), FGameplayEventData());
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::OnMontageEndedForInstance(
	UAnimMontage* Montage, bool bInterrupted, FGGYGOMontagePlayGuardIdentity Original)
{
	if (MatchesOriginalMontageCallback(Original)) { OnMontageEnded(Montage, bInterrupted); }
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::OnMontageSectionChangedForInstance(
	UAnimMontage* Montage, FName SectionName, bool bLooped, FGGYGOMontagePlayGuardIdentity Original)
{
	if (Montage != MontageToPlay || !MatchesOriginalMontageCallback(Original)) { return; }
	FGGYGOMontageSectionFact Fact;
	Fact.Montage = Montage;
	Fact.MontageInstanceId = Original.CreatedInstanceId;
	Fact.SectionName = SectionName;
	Fact.bLooped = bLooped;
	DispatchOriginalSectionFact(Fact, Original);
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::OnMontageEnded(UAnimMontage* Montage, bool bInterrupted)
{
	if (bEndingTask || bCancellationRequested || Montage != MontageToPlay) { return; }
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(this);
	ReleaseRootMotionScaleLease();
	// Dispatch authenticates the original Ended fact, not current ASC ownership or an active instance.
	if (!bInterrupted)
	{
		DispatchOriginalCallback(ENativeCallback::Completed, FGameplayTag(), FGameplayEventData());
	}
	if (UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get())
	{
		if (!Task->bEndingTask && Task->GetState() != EGameplayTaskState::Finished) { Task->EndTask(); }
	}
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::OnGameplayEvent(FGameplayTag EventTag, const FGameplayEventData* Payload)
{
	if (!Payload || !ShouldBroadcastAbilityTaskDelegates() || !IsNotifyValid())
	{
		return;
	}
	if (const UAnimMontage* Source = Cast<UAnimMontage>(Payload->OptionalObject))
	{
		if (Source != MontageToPlay) { return; }
		// GameplayEventWindow 把本地播放实例 ID+1 放入 Magnitude；0 表示无上下文。
		if (Payload->EventMagnitude > 0.0f && MontageInstanceId != INDEX_NONE
			&& FMath::RoundToInt(Payload->EventMagnitude) - 1 != MontageInstanceId) { return; }
	}

	// 拷一份负载再广播：原始指针指向 ASC 内部的临时对象，
	// 广播过程中若有人结束了任务，那块内存就失效了。
	FGameplayEventData TempData = *Payload;
	TempData.EventTag = EventTag;

	DispatchOriginalCallback(ENativeCallback::EventReceived, EventTag, MoveTemp(TempData));
}

FString UGGYGOAbilityTask_PlayMontageAndWaitForEvent::GetDebugString() const
{
	const UAnimMontage* PlayingMontage = nullptr;
	if (const UAnimInstance* AnimInstance = ActivatedAnimInstance.Get())
	{
		PlayingMontage = AnimInstance->Montage_IsActive(MontageToPlay)
			? MontageToPlay.Get()
			: AnimInstance->GetCurrentActiveMontage();
	}

	return FString::Printf(TEXT("PlayMontageAndWaitForEvent. MontageToPlay: %s (playing %s)"),
		*GetNameSafe(MontageToPlay), *GetNameSafe(PlayingMontage));
}

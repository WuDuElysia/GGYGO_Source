/**
 * @file GGYGOAbilityTask_PlayMontageAndWaitForEvent.cpp
 * @brief Montage 播放与事件等待实现
 */
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/Tasks/GGYGORootMotionScaleLease.h"
#include "Animation/Notifies/GGYGOAnimNotifyState_GameplayEventWindow.h"
#include "Animation/Runtime/GGYGOMontageGuardAnimInstance.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "AnimNotifies/AnimNotify_PlayMontageNotify.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Misc/ScopeExit.h"

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
	bool ResolveNativeMontageNotify(const UAnimMontage* Montage, FName NotifyName,
		const FAnimNotifyEvent*& OutEvent, FGGYGOMontageNotifyDeclaration& OutDeclaration, FString& OutDiagnostic)
	{
		OutEvent = nullptr;
		OutDeclaration = {};
		OutDiagnostic.Reset();
		if (!IsInGameThread() || !IsValid(Montage) || NotifyName.IsNone())
		{
			OutDiagnostic = TEXT("Native Montage Notify resolution requires GameThread, a valid Montage and a nonempty name.");
			return false;
		}
		const float Length = Montage->GetPlayLength();
		if (!FMath::IsFinite(Length) || Length <= 0.0f)
		{
			OutDiagnostic = TEXT("Native Montage Notify resolution requires a finite positive Montage length.");
			return false;
		}
		const FAnimNotifyEvent* Matched = nullptr;
		for (const FAnimNotifyEvent& Event : Montage->Notifies)
		{
			const UAnimNotify_PlayMontageNotifyWindow* Window = Cast<UAnimNotify_PlayMontageNotifyWindow>(Event.NotifyStateClass.Get());
			if (Window && FName(*Window->UAnimNotify_PlayMontageNotifyWindow::GetNotifyName_Implementation()) == NotifyName)
			{
				OutDiagnostic = TEXT("The configured name also identifies a Montage Notify Window; a single native point is required.");
				return false;
			}
			const UAnimNotify_PlayMontageNotify* Notify = Cast<UAnimNotify_PlayMontageNotify>(Event.Notify.Get());
			// Read the native Broadcast name without invoking a Blueprint display-name override.
			if (!Notify || FName(*Notify->UAnimNotify_PlayMontageNotify::GetNotifyName_Implementation()) != NotifyName) { continue; }
			if (Notify->GetClass() != UAnimNotify_PlayMontageNotify::StaticClass()
				|| Event.NotifyStateClass || Event.GetLinkedMontage() != Montage
				|| !Event.IsBranchingPoint() || Event.GetDuration() != 0.0f)
			{
				OutDiagnostic = TEXT("The configured name must identify the stock UAnimNotify_PlayMontageNotify native point, not a state or subclass.");
				return false;
			}
			if (Matched)
			{
				OutDiagnostic = TEXT("The configured native Montage Notify point is duplicated.");
				return false;
			}
			const float AuthoredPosition = Event.GetTime();
			const float TriggerPosition = Event.GetTriggerTime();
			if (!FMath::IsFinite(AuthoredPosition) || AuthoredPosition < 0.0f || AuthoredPosition > Length
				|| !FMath::IsFinite(Event.TriggerTimeOffset)
				|| !FMath::IsFinite(TriggerPosition) || TriggerPosition < 0.0f || TriggerPosition > Length)
			{
				OutDiagnostic = FString::Printf(TEXT("原生 Montage Notify '%s' 的作者时间、offset 或触发时间无效。"), *NotifyName.ToString());
				return false;
			}
			Matched = &Event;
		}
		if (!Matched)
		{
			OutDiagnostic = TEXT("The configured stock native Montage Notify point is missing.");
			return false;
		}
		// UE 按精确 float 时间检测 BranchingPoint Begin/StateEnd 碰撞，碰撞会漏发其中一项。
		// 只拒绝涉及所选点的碰撞；不修改作者位置，也不把合法 OffsetBefore 改成零。
		const float TriggerPosition = Matched->GetTriggerTime();
		for (int32 Index = 0; Index < Montage->Notifies.Num(); ++Index)
		{
			const FAnimNotifyEvent& Other = Montage->Notifies[Index];
			if (!Other.IsBranchingPoint()) { continue; }
			if ((&Other != Matched && Other.GetTriggerTime() == TriggerPosition)
				|| (Other.NotifyStateClass && Other.GetEndTriggerTime() == TriggerPosition))
			{
				OutDiagnostic = FString::Printf(TEXT("原生 Montage Notify '%s' 在 %.9g 与 BranchingPoint[%d] 的实际触发时间碰撞。"),
					*NotifyName.ToString(), TriggerPosition, Index);
				return false;
			}
		}
		OutEvent = Matched; // 数组指针只借用于本调用栈，不跨资产变更保留。
		OutDeclaration.NotifyName = NotifyName;
		OutDeclaration.AuthoredPositionSeconds = Matched->GetTime();
		OutDeclaration.TriggerPositionSeconds = TriggerPosition;
		OutDeclaration.TriggerTimeOffsetSeconds = Matched->TriggerTimeOffset;
		return true;
	}

	bool HasSameGGYGOMontageTaskNotifyTiming(const FGGYGOMontageNotifyDeclaration& First,
		const FGGYGOMontageNotifyDeclaration& Second)
	{
		return First.NotifyName == Second.NotifyName
			&& First.AuthoredPositionSeconds == Second.AuthoredPositionSeconds
			&& First.TriggerPositionSeconds == Second.TriggerPositionSeconds
			&& First.TriggerTimeOffsetSeconds == Second.TriggerTimeOffsetSeconds;
	}

	bool StopOriginalMontageInstance(const FGGYGOMontagePlayGuardIdentity& Identity,
		const FGGYGOAbilityMontagePlayback& Playback,
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
		if (ASC && ASC->IsPlaybackCurrent(Playback) && ActiveInstance == Instance)
		{
			// 仍是本激活的当前播放且实例一致：走 GAS 现有的停止与复制路径。
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
	if (HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) || bEndingTask || bCancellationRequested || bPoseFailureRequested
		|| GetState() != EGameplayTaskState::AwaitingActivation)
	{
		Reason = TEXT("Task is not in its original pre-Ready AwaitingActivation state");
	}
	else if (NativeCallbackRegistration.IsValid()) { Reason = TEXT("a native callback package is already registered"); }
	else if (!Callbacks.OnCompleted.IsBound() && !Callbacks.OnBlendOut.IsBound()
		&& !Callbacks.OnInterrupted.IsBound() && !Callbacks.OnCancelled.IsBound()
		&& !Callbacks.EventReceived.IsBound() && !Callbacks.SectionReceived.IsBound()
		&& !Callbacks.NotifyReceived.IsBound() && !Callbacks.WindowReceived.IsBound() && !Callbacks.OnFailed.IsBound())
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
		|| Task->bCancellationRequested || Task->bPoseFailureRequested || Task->GetState() != EGameplayTaskState::AwaitingActivation
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
	if (HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) || bEndingTask || bPoseFailureRequested
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
	if (HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) || bEndingTask || bPoseFailureRequested
		|| GetState() == EGameplayTaskState::Finished || !ShouldBroadcastAbilityTaskDelegates()
		|| !MatchesOriginalMontageCallback(Original)) { return false; }
	const UGGYGOMontageGuardAnimInstance* Guard = Cast<UGGYGOMontageGuardAnimInstance>(ActivatedAnimInstance.Get());
	// The engine dispatches queued section facts after blend-out and before Ended. These facts,
	// like natural Ended, do not require current ASC playback ownership or a surviving instance.
	return Guard && Guard->IsMontagePlayGuardIdentityCurrent(Original) && IsActivatedActorInfoCurrent();
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::DispatchOriginalCallback(
	ENativeCallback Kind, FGameplayTag EventTag, FGameplayEventData EventData, bool bFromOwnerCancellation)
{
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(this);
	const TWeakObjectPtr<UGameplayAbility> OriginalAbility(Ability);
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalASC(AbilitySystemComponent.Get());
	const FGGYGOMontagePlayGuardIdentity OriginalGuard = OriginalPlayback.Guard;
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
	if (NativeSnapshot.IsValid() && (!bFromOwnerCancellation || NativeSnapshot->Callbacks.bNotifyNativeOnOwnerCancellation))
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
	if (!CanDispatchOriginalInstanceFact(OriginalPlayback.Guard)) { return false; }
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

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageNotify(
	const UAnimMontage* Montage, FName NotifyName, float& OutNotifyPositionSeconds, FString& OutDiagnostic)
{
	OutNotifyPositionSeconds = 0.0f;
	FGGYGOMontageNotifyDeclaration Declaration;
	if (!ResolveOriginalMontageNotifyDeclaration(Montage, NotifyName, Declaration, OutDiagnostic)) { return false; }
	OutNotifyPositionSeconds = Declaration.TriggerPositionSeconds;
	return true;
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageNotifyDeclaration(
	const UAnimMontage* Montage, FName NotifyName, FGGYGOMontageNotifyDeclaration& OutDeclaration, FString& OutDiagnostic)
{
	const FAnimNotifyEvent* Event = nullptr;
	if (!ResolveNativeMontageNotify(Montage, NotifyName, Event, OutDeclaration, OutDiagnostic)) { return false; }
	OutDeclaration.NativeNotify = Event->Notify;
	return true;
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageWindowAtIndex(
	const UAnimMontage* Montage, int32 NotifyIndex, float RangeStartSeconds, float RangeEndSeconds,
	FGGYGOMontageWindowDeclaration& OutDeclaration, FString& OutDiagnostic)
{
	OutDeclaration = {};
	OutDiagnostic.Reset();
	if (!IsValid(Montage) || !Montage->Notifies.IsValidIndex(NotifyIndex)
		|| !FMath::IsFinite(RangeStartSeconds) || !FMath::IsFinite(RangeEndSeconds)
		|| RangeStartSeconds < 0.0f || RangeEndSeconds <= RangeStartSeconds)
	{
		OutDiagnostic = TEXT("作者窗口需要原 Montage、有效条目及有限正向绑定范围。");
		return false;
	}
	const FAnimNotifyEvent& Event = Montage->Notifies[NotifyIndex];
	auto* Notify = Cast<UGGYGOAnimNotifyState_GameplayEventWindow>(Event.NotifyStateClass.Get());
	const float Begin = Event.GetTime();
	const float End = Begin + Event.GetDuration();
	const float TriggerBegin = Event.GetTriggerTime();
	const float TriggerEnd = Event.GetEndTriggerTime();
	if (!Notify || !Notify->GetBeginEventTag().IsValid() || !Notify->GetEndEventTag().IsValid()
		|| Event.GetLinkedMontage() != Montage || !FMath::IsFinite(Begin) || !FMath::IsFinite(End)
		|| !FMath::IsFinite(TriggerBegin) || !FMath::IsFinite(TriggerEnd)
		|| Begin < RangeStartSeconds || End <= Begin || End > RangeEndSeconds)
	{
		OutDiagnostic = TEXT("原作者窗口的标签、链接或时间范围无效。");
		return false;
	}
	OutDeclaration.Montage = const_cast<UAnimMontage*>(Montage);
	OutDeclaration.BeginEventTag = Notify->GetBeginEventTag();
	OutDeclaration.EndEventTag = Notify->GetEndEventTag();
	OutDeclaration.BeginSeconds = Begin;
	OutDeclaration.EndSeconds = End;
	OutDeclaration.BeginTriggerSeconds = TriggerBegin;
	OutDeclaration.EndTriggerSeconds = TriggerEnd;
	OutDeclaration.bQueued = Event.MontageTickType == EMontageNotifyTickType::Queued;
	OutDeclaration.bNoMergeOnConcurrentPlay = (Notify->NotifyStateBehaviorFlags
		& static_cast<uint8>(EAnimNotifyStateBehaviorFlags::NoMergeOnConcurrentPlay)) != 0;
	OutDeclaration.NativeNotify = Notify;
	OutDeclaration.NotifyIndex = NotifyIndex;
	OutDeclaration.RangeStartSeconds = RangeStartSeconds;
	OutDeclaration.RangeEndSeconds = RangeEndSeconds;
	return true;
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageWindowDeclaration(
	const UAnimMontage* Montage, FGameplayTag BeginEventTag, FGameplayTag EndEventTag,
	float RangeStartSeconds, float RangeEndSeconds, FGGYGOMontageWindowDeclaration& OutDeclaration,
	FString& OutDiagnostic)
{
	OutDeclaration = {};
	OutDiagnostic.Reset();
	if (!IsValid(Montage) || !BeginEventTag.IsValid() || !EndEventTag.IsValid())
	{
		OutDiagnostic = TEXT("唯一窗口解析需要原 Montage 和明确 Begin/End 标签。");
		return false;
	}
	int32 Match = INDEX_NONE;
	for (int32 Index = 0; Index < Montage->Notifies.Num(); ++Index)
	{
		const auto* Notify = Cast<UGGYGOAnimNotifyState_GameplayEventWindow>(Montage->Notifies[Index].NotifyStateClass.Get());
		if (!Notify) { continue; }
		const FGameplayTag Begin = Notify->GetBeginEventTag();
		const FGameplayTag End = Notify->GetEndEventTag();
		if (Begin != BeginEventTag && Begin != EndEventTag && End != BeginEventTag && End != EndEventTag) { continue; }
		if (Begin != BeginEventTag || End != EndEventTag || Match != INDEX_NONE)
		{
			OutDiagnostic = TEXT("必需唯一窗口存在重复条目或标签配对冲突。");
			return false;
		}
		Match = Index;
	}
	if (Match == INDEX_NONE)
	{
		OutDiagnostic = TEXT("原 Montage 没有请求的必需窗口。");
		return false;
	}
	return ResolveOriginalMontageWindowAtIndex(Montage, Match, RangeStartSeconds, RangeEndSeconds, OutDeclaration, OutDiagnostic);
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageWindows(
	const UAnimMontage* Montage, const FGameplayTagContainer& Tags, float RangeStartSeconds, float RangeEndSeconds,
	TArray<FGGYGOMontageWindowDeclaration>& OutDeclarations, FString& OutDiagnostic)
{
	OutDeclarations.Reset();
	OutDiagnostic.Reset();
	if (!IsValid(Montage)) { OutDiagnostic = TEXT("窗口解析缺少原 Montage。"); return false; }
	TArray<FGGYGOMontageWindowDeclaration> Declarations;
	for (int32 Index = 0; Index < Montage->Notifies.Num(); ++Index)
	{
		const auto* Notify = Cast<UGGYGOAnimNotifyState_GameplayEventWindow>(Montage->Notifies[Index].NotifyStateClass.Get());
		if (!Notify || (!Notify->GetBeginEventTag().MatchesAny(Tags) && !Notify->GetEndEventTag().MatchesAny(Tags))) { continue; }
		FGGYGOMontageWindowDeclaration Declaration;
		if (!ResolveOriginalMontageWindowAtIndex(Montage, Index, RangeStartSeconds, RangeEndSeconds,
			Declaration, OutDiagnostic)) { return false; }
		Declarations.Add(MoveTemp(Declaration));
	}
	OutDeclarations = MoveTemp(Declarations);
	return true;
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::IsOriginalMontageWindowDeclarationCurrent(
	const FGGYGOMontageWindowDeclaration& Declaration) const
{
	FGGYGOMontageWindowDeclaration Actual;
	FString Diagnostic;
	return Declaration.Montage == MontageToPlay && IsValid(Declaration.NativeNotify)
		&& ResolveOriginalMontageWindowAtIndex(MontageToPlay, Declaration.NotifyIndex,
			Declaration.RangeStartSeconds, Declaration.RangeEndSeconds, Actual, Diagnostic)
		&& Actual.HasSameWindow(Declaration);
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ConfigureOriginalMontageWindows(
	const TArray<FGGYGOMontageWindowDeclaration>& Declarations, FString& OutDiagnostic)
{
	OutDiagnostic.Reset();
	if (!IsInGameThread() || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) || bEndingTask
		|| bCancellationRequested || bPoseFailureRequested || bMontageWindowsConfigured
		|| GetState() != EGameplayTaskState::AwaitingActivation)
	{
		OutDiagnostic = TEXT("窗口声明只允许在原 Task Ready 前提交一次。");
		return false;
	}
	TSet<int32> Indices;
	for (const auto& Declaration : Declarations)
	{
		if (!IsOriginalMontageWindowDeclarationCurrent(Declaration) || Indices.Contains(Declaration.NotifyIndex))
		{
			OutDiagnostic = TEXT("窗口声明与原资产不符，或同次声明集中有重复原条目。");
			return false;
		}
		Indices.Add(Declaration.NotifyIndex);
	}
	ConfiguredMontageWindowDeclarations = Declarations;
	bMontageWindowsConfigured = true;
	for (const auto& Declaration : Declarations)
	{
		EventTags.AddTag(Declaration.BeginEventTag);
		EventTags.AddTag(Declaration.EndEventTag);
	}
	return true;
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::TryGetOriginalMontageWindowSnapshot(
	FGameplayTag BeginEventTag, FGameplayTag EndEventTag, FGGYGOMontageWindowDeclaration& OutDeclaration,
	FGGYGOMontageSectionSnapshot& OutPlayback) const
{
	check(IsInGameThread());
	OutDeclaration = {};
	OutPlayback = {};
	const FGGYGOMontageWindowDeclaration* Match = nullptr;
	for (const auto& Declaration : ConfiguredMontageWindowDeclarations)
	{
		if (Declaration.BeginEventTag != BeginEventTag || Declaration.EndEventTag != EndEventTag) { continue; }
		if (Match) { return false; }
		Match = &Declaration;
	}
	if (!Match || !IsOriginalMontageWindowDeclarationCurrent(*Match)
		|| !TryGetOriginalSectionSnapshot(OutPlayback)) { return false; }
	OutDeclaration = *Match;
	return true;
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ConfigureOriginalMontageNotify(FName InName, FString& OutDiagnostic)
{
	return ConfigureOriginalMontageNotifies({InName}, OutDiagnostic);
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ConfigureOriginalMontageNotifies(
	const TArray<FName>& InNames, FString& OutDiagnostic)
{
	OutDiagnostic.Reset();
	if (!IsInGameThread() || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) || bEndingTask
		|| bCancellationRequested || bPoseFailureRequested || GetState() != EGameplayTaskState::AwaitingActivation
		|| !ConfiguredMontageNotifyDeclarations.IsEmpty())
	{
		OutDiagnostic = TEXT("Native Montage Notify configuration is allowed once, on the original pre-Ready AwaitingActivation task.");
		return false;
	}
	if (InNames.IsEmpty())
	{
		OutDiagnostic = TEXT("原生 Montage Notify 配置需要非空名字集。");
		return false;
	}
	TArray<FGGYGOMontageNotifyDeclaration> Declarations;
	Declarations.Reserve(InNames.Num());
	for (FName Name : InNames)
	{
		if (Name.IsNone() || Declarations.ContainsByPredicate([Name](const FGGYGOMontageNotifyDeclaration& Item)
			{ return Item.NotifyName == Name; }))
		{
			OutDiagnostic = FString::Printf(TEXT("原生 Montage Notify 配置名字 '%s' 为空或重复。"), *Name.ToString());
			return false;
		}
		FGGYGOMontageNotifyDeclaration Declaration;
		if (!ResolveOriginalMontageNotifyDeclaration(MontageToPlay, Name, Declaration, OutDiagnostic)) { return false; }
		Declarations.Add(MoveTemp(Declaration));
	}
	// 解析不调用外部代码；全部声明通过后才提交唯一配置集，失败不占用配置机会。
	ConfiguredMontageNotifyDeclarations = MoveTemp(Declarations);
	return true;
}

const FGGYGOMontageNotifyDeclaration* UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FindConfiguredMontageNotify(FName NotifyName) const
{
	return ConfiguredMontageNotifyDeclarations.FindByPredicate([NotifyName](const FGGYGOMontageNotifyDeclaration& Item)
		{ return Item.NotifyName == NotifyName; });
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::IsOriginalMontageNotifySourceCurrent(
	const FGGYGOMontageNotifyDeclaration& Declaration) const
{
	if (!IsValid(Declaration.NativeNotify)) { return false; }
	const FAnimNotifyEvent* Event = nullptr;
	FGGYGOMontageNotifyDeclaration Actual;
	FString Diagnostic;
	return ResolveNativeMontageNotify(MontageToPlay, Declaration.NotifyName, Event, Actual, Diagnostic)
		&& Event->Notify == Declaration.NativeNotify && HasSameGGYGOMontageTaskNotifyTiming(Actual, Declaration);
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::AreOriginalMontageNotifySourcesCurrent() const
{
	for (const FGGYGOMontageNotifyDeclaration& Declaration : ConfiguredMontageNotifyDeclarations)
	{
		if (!IsOriginalMontageNotifySourceCurrent(Declaration)) { return false; }
	}
	return !ConfiguredMontageNotifyDeclarations.IsEmpty();
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::TryGetOriginalMontageNotifySnapshot(
	FGGYGOMontageNotifyFact& OutFact, float& OutPositionSeconds) const
{
	check(IsInGameThread());
	OutFact = {};
	OutPositionSeconds = 0.0f;
	if (ConfiguredMontageNotifyDeclarations.Num() != 1) { return false; }
	return TryGetOriginalMontageNotifySnapshotByName(ConfiguredMontageNotifyDeclarations[0].NotifyName, OutFact, OutPositionSeconds);
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::TryGetOriginalMontageNotifySnapshotByName(
	FName NotifyName, FGGYGOMontageNotifyFact& OutFact, float& OutPositionSeconds) const
{
	check(IsInGameThread());
	OutFact = {};
	OutPositionSeconds = 0.0f;
	const FGGYGOMontageNotifyDeclaration* Declaration = FindConfiguredMontageNotify(NotifyName);
	if (!Declaration || !CanDispatchOriginalInstanceFact(OriginalPlayback.Guard)
		|| !IsOriginalMontageNotifySourceCurrent(*Declaration)) { return false; }
	const FAnimMontageInstance* Instance = GetTaskMontageInstance();
	if (!Instance || !FMath::IsFinite(Instance->GetPosition())) { return false; }
	OutFact.Montage = MontageToPlay;
	OutFact.Mesh = ActivatedMesh.Get();
	OutFact.MontageInstanceId = Instance->GetInstanceID();
	OutFact.NotifyName = Declaration->NotifyName;
	OutFact.NotifyPositionSeconds = Declaration->TriggerPositionSeconds;
	OutFact.NotifyAuthoredPositionSeconds = Declaration->AuthoredPositionSeconds;
	OutFact.NotifyTriggerTimeOffsetSeconds = Declaration->TriggerTimeOffsetSeconds;
	OutPositionSeconds = Instance->GetPosition();
	return true;
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::CanDispatchOriginalMontageNotifyFact(
	const FGGYGOMontageNotifyFact& Fact, const FGGYGOMontagePlayGuardIdentity& Original) const
{
	const FGGYGOMontageNotifyDeclaration* Declaration = FindConfiguredMontageNotify(Fact.NotifyName);
	return Declaration && CanDispatchOriginalInstanceFact(Original) && IsOriginalMontageNotifySourceCurrent(*Declaration)
		&& GetTaskMontageInstance() && Fact.Montage == MontageToPlay && Fact.Mesh == ActivatedMesh.Get()
		&& Fact.MontageInstanceId == Original.CreatedInstanceId
		&& Fact.NotifyPositionSeconds == Declaration->TriggerPositionSeconds
		&& Fact.NotifyAuthoredPositionSeconds == Declaration->AuthoredPositionSeconds
		&& Fact.NotifyTriggerTimeOffsetSeconds == Declaration->TriggerTimeOffsetSeconds;
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::DispatchValidatedOriginalMontageNotifyFact(
	const FGGYGOMontageNotifyFact& Fact, const FGGYGOMontagePlayGuardIdentity& Original)
{
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(this);
	const TWeakObjectPtr<UGameplayAbility> OriginalAbility(Ability);
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalASC(AbilitySystemComponent.Get());
	const auto Recheck = [&]() -> UGGYGOAbilityTask_PlayMontageAndWaitForEvent*
	{
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get();
		return Task && OriginalAbility.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGameplayAbility>(Task->Ability))
			&& OriginalASC.HasSameIndexAndSerialNumber(TWeakObjectPtr<UAbilitySystemComponent>(Task->AbilitySystemComponent.Get()))
			&& Task->CanDispatchOriginalMontageNotifyFact(Fact, Original) ? Task : nullptr;
	};
	TSharedPtr<FNativeCallbackRegistration> NativeSnapshot = NativeCallbackRegistration;
	if (NativeSnapshot.IsValid()) { NativeSnapshot->Callbacks.NotifyReceived.ExecuteIfBound(Fact); }
	NativeSnapshot.Reset(); // Capture destructors are external code too; recheck after they return.
	if (UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = Recheck()) { Task->NotifyReceived.Broadcast(Fact); }
	// No task access after BP; the caller may retire this task and launch a same-asset successor.
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::OnOriginalMontageNotifyBegin(
	FName NotifyName, const FBranchingPointNotifyPayload& Payload)
{
	const FGGYGOMontageNotifyDeclaration* Declaration = FindConfiguredMontageNotify(NotifyName);
	if (!Declaration || !IsValid(Declaration->NativeNotify)
		|| Payload.MontageInstanceID == INDEX_NONE
		|| Payload.MontageInstanceID != MontageInstanceId || Payload.SkelMeshComponent != ActivatedMesh.Get()
		|| Payload.SequenceAsset != MontageToPlay || !Payload.NotifyEvent) { return; }
	const FAnimNotifyEvent* Event = nullptr;
	FGGYGOMontageNotifyDeclaration Actual;
	FString Diagnostic;
	if (!ResolveNativeMontageNotify(MontageToPlay, NotifyName, Event, Actual, Diagnostic)
		|| Event != Payload.NotifyEvent || Event->Notify != Declaration->NativeNotify
		|| !HasSameGGYGOMontageTaskNotifyTiming(Actual, *Declaration)) { return; }
	const FGGYGOMontagePlayGuardIdentity Original = OriginalPlayback.Guard;
	if (!CanDispatchOriginalInstanceFact(Original)) { return; }
	const FAnimMontageInstance* Instance = GetTaskMontageInstance();
	if (!Instance || !FMath::IsFinite(Instance->GetPosition())) { return; }
	// Payload, authored declaration and original instance were checked without an external call.
	// Copy the fact once; never forward or retain the borrowed engine payload.
	FGGYGOMontageNotifyFact Fact;
	Fact.Montage = MontageToPlay;
	Fact.Mesh = ActivatedMesh.Get();
	Fact.MontageInstanceId = Instance->GetInstanceID();
	Fact.NotifyName = Declaration->NotifyName;
	Fact.NotifyPositionSeconds = Declaration->TriggerPositionSeconds;
	Fact.NotifyAuthoredPositionSeconds = Declaration->AuthoredPositionSeconds;
	Fact.NotifyTriggerTimeOffsetSeconds = Declaration->TriggerTimeOffsetSeconds;
	DispatchValidatedOriginalMontageNotifyFact(Fact, Original);
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
	if (bEndingTask || bCancellationRequested || bPoseFailureRequested || GetState() == EGameplayTaskState::Finished) { return; }
	if (InFlightMontagePlayCleanup.IsValid() || OriginalPlayback.Guard.CallId != 0)
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
	for (const auto& Declaration : ConfiguredMontageWindowDeclarations)
	{
		if (!IsOriginalMontageWindowDeclarationCurrent(Declaration))
		{
			FailAndEndTask(TEXT("配置的原作者窗口在 Ready 前已失效"));
			return;
		}
	}
	if (!ConfiguredMontageWindowDeclarations.IsEmpty() && !WindowReceived.IsBound()
		&& !(NativeCallbackRegistration.IsValid() && NativeCallbackRegistration->Callbacks.WindowReceived.IsBound()))
	{
		FailAndEndTask(TEXT("已配置原作者窗口但没有 Ready 前安装的 typed 消费者"));
		return;
	}
	if (!ConfiguredMontageNotifyDeclarations.IsEmpty())
	{
		if (!AreOriginalMontageNotifySourcesCurrent())
		{
			FailAndEndTask(TEXT("配置的原生 Montage Notify 声明在 Ready 前已失效"));
			return;
		}
		if (!NotifyReceived.IsBound() && !(NativeCallbackRegistration.IsValid()
			&& NativeCallbackRegistration->Callbacks.NotifyReceived.IsBound()))
		{
			FailAndEndTask(TEXT("配置的原生 Montage Notify 没有 pre-Ready native 或 Blueprint 消费者"));
			return;
		}
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

	ActivatedAbility = Ability;
	ActivatedMesh = ActorInfo->SkeletalMeshComponent;
	ActivatedCharacter = Cast<ACharacter>(AvatarActor);
	ActivatedAnimInstance = AnimInstance;
	ActivatedASC = ASC;
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(this);
	const TWeakObjectPtr<UGameplayAbility> OriginalAbility(Ability);
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalASC(ASC);
	const TWeakObjectPtr<UAnimMontage> OriginalMontage(MontageToPlay);
	// A rejected Acquire resets its ticket. Preserve the actual unambiguous original slot
	// before querying, so startup failure still identifies the caller's original resource.
	const FName OriginalSlot = MontageToPlay->SlotAnimTracks.Num() == 1
		? MontageToPlay->SlotAnimTracks[0].SlotName : NAME_None;
	if (UGGYGOMontageGuardAnimInstance* Guard = Cast<UGGYGOMontageGuardAnimInstance>(AnimInstance))
	{
		FGGYGOActionPoseContractTicket AcquiredTicket;
		FString Diagnostic;
		const EGGYGOActionPoseContractAcquireResult Acquired = Guard->AcquireActionPoseContract(
			OriginalMontage.Get(), AcquiredTicket, Diagnostic);
		// Acquire waits at the native parallel barrier. Any completion callback may have
		// ended this task/GA or changed ActorInfo; never install a ticket through that tail.
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get();
		if (!Task || Task->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) || Task->bEndingTask
			|| Task->bCancellationRequested || Task->bPoseFailureRequested
			|| Task->GetState() == EGameplayTaskState::Finished) { return; }
		if (!OriginalAbility.IsValid() || !OriginalAbility->IsActive() || Task->Ability != OriginalAbility.Get())
		{
			Task->EndTask();
			return;
		}
		if (!Task->IsActivatedActorInfoCurrent())
		{
			Task->FailActionPoseContract(EGGYGOMontageTaskFailureStage::Startup, OriginalSlot,
				TEXT("Original Task/Ability/ASC/ActorInfo/Owner/Avatar/Mesh/Anim context changed during pose contract Acquire."));
			return;
		}
		if (Acquired == EGGYGOActionPoseContractAcquireResult::Rejected
			|| Acquired == EGGYGOActionPoseContractAcquireResult::Unavailable)
		{
			Task->FailActionPoseContract(EGGYGOMontageTaskFailureStage::Startup, OriginalSlot,
				FString::Printf(TEXT("Pose contract Acquire result=%d; %s"), int32(Acquired), *Diagnostic));
			return;
		}
		if (Acquired == EGGYGOActionPoseContractAcquireResult::RequiredReady)
		{
			if (OriginalSlot.IsNone() || !AcquiredTicket.IsValid() || AcquiredTicket.GetSlotName() != OriginalSlot)
			{
				Task->FailActionPoseContract(EGGYGOMontageTaskFailureStage::Startup, OriginalSlot,
					TEXT("RequiredReady did not issue a valid original ticket for this Montage's unique actual slot."));
				return;
			}
			if (!Task->OnFailed.IsBound() && !(Task->NativeCallbackRegistration.IsValid()
				&& Task->NativeCallbackRegistration->Callbacks.OnFailed.IsBound()))
			{
				Task->FailActionPoseContract(EGGYGOMontageTaskFailureStage::Startup, OriginalSlot,
					TEXT("Required pose contract has no pre-Ready native or Blueprint OnFailed consumer; playback was not started."));
				return;
			}
			Task->ActionPoseContractTicket = MoveTemp(AcquiredTicket);
			// PerformActivation registers ticking tasks after Activate returns. Required
			// playback failures are therefore polled after the GA fixes its Ready result ID.
			Task->bTickingTask = true;
		}
		else if (Acquired != EGGYGOActionPoseContractAcquireResult::ExplicitNotRequired)
		{
			Task->FailActionPoseContract(EGGYGOMontageTaskFailureStage::Startup, OriginalSlot,
				FString::Printf(TEXT("Unknown pose contract Acquire result=%d; %s"), int32(Acquired), *Diagnostic));
			return;
		}
	}
	const FGameplayAbilityActivationInfo OriginalActivationInfo = Ability->GetCurrentActivationInfo();
	const bool bStopOnOwnerEnd = bStopWhenAbilityEnds;
	const TSharedRef<FInFlightMontagePlayCleanup> Cleanup = MakeShared<FInFlightMontagePlayCleanup>();
	InFlightMontagePlayCleanup = Cleanup;

	// Both subscriptions belong to this original Task, including cancellation during native play.
	CancelledHandle = Ability->OnGameplayAbilityCancelled.Add(
		FOnGameplayAbilityCancelled::FDelegate::CreateUObject(this, &ThisClass::OnAbilityCancelled));
	EventHandle = ASC->AddGameplayEventTagContainerDelegate(EventTags,
		FGameplayEventTagMulticastDelegate::FDelegate::CreateUObject(this, &ThisClass::OnGameplayEvent));

	const FGGYGOAbilityMontagePlaybackResult Result = ASC->PlayAbilityMontage(
		OriginalAbility.Get(), OriginalActivationInfo, OriginalMontage.Get(), Rate, StartSection,
		StartTimeSeconds, [OriginalTask, OriginalAbility, OriginalASC]()
		{
			const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get();
			// Pure Task liveness only. The ASC owns native activation/playback authentication.
			return Task && !Task->bEndingTask && !Task->bCancellationRequested && !Task->bPoseFailureRequested
				&& Task->GetState() != EGameplayTaskState::Finished
				&& Task->Ability == OriginalAbility.Get() && OriginalAbility.IsValid()
				&& Task->AbilitySystemComponent.Get() == OriginalASC.Get()
				&& Task->IsActivatedActorInfoCurrent();
		});

	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get();
	const bool bTaskOpen = Task && !Task->bEndingTask && !Task->bCancellationRequested && !Task->bPoseFailureRequested
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
	if (!Result.Playback.HasPlayback() || Result.Guard.Outcome != EGGYGOMontagePlayGuardOutcome::Accepted
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
	Task->MontageInstanceId = Result.Guard.Identity.CreatedInstanceId;
	FAnimMontageInstance* Instance = Task->GetTaskMontageInstance();
	if (!Instance || !Instance->IsActive() || !Instance->IsPlaying())
	{
		Task->StopPlayingMontage();
		Task = OriginalTask.Get();
		if (Task && !Task->bEndingTask) { Task->FailAndEndTask(TEXT("原 Guard 实例已失效，无法安装任务委托"), &Result); }
		return;
	}
	if (!Task->ConfiguredMontageNotifyDeclarations.IsEmpty() && !Task->AreOriginalMontageNotifySourcesCurrent())
	{
		Task->StopPlayingMontage();
		Task = OriginalTask.Get();
		if (Task && !Task->bEndingTask && Task->GetState() != EGameplayTaskState::Finished)
		{
			Task->FailAndEndTask(TEXT("原生播放外调返回后配置的 Montage Notify 声明已改变"));
		}
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
	if (!Task->ConfiguredMontageNotifyDeclarations.IsEmpty())
	{
		// This engine multicast has no handle: detach only the original AnimInstance/task/function pair.
		AnimInstance->OnPlayMontageNotifyBegin.AddDynamic(Task, &ThisClass::OnOriginalMontageNotifyBegin);
	}

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

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::TickTask(float /*DeltaTime*/)
{
	check(IsInGameThread());
	// This task performs no time accumulation, animation/position evaluation or movement.
	// bTickingTask is enabled only by RequiredReady and retained through native deactivation.
	if (!bTickingTask || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) || bEndingTask
		|| bCancellationRequested || bPoseFailureRequested || GetState() != EGameplayTaskState::Active) { return; }
	const FName OriginalSlot = ActionPoseContractTicket.GetSlotName();
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(this);
	const TWeakObjectPtr<UGameplayAbility> OriginalAbility = ActivatedAbility;
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalASC = ActivatedASC;
	const FGGYGOMontagePlayGuardIdentity OriginalGuard = OriginalPlayback.Guard;
	const FGGYGOActionPoseContractTicket OriginalTicket = ActionPoseContractTicket;
	UGGYGOMontageGuardAnimInstance* Guard = Cast<UGGYGOMontageGuardAnimInstance>(ActivatedAnimInstance.Get());
	if (!Guard || !IsActivatedActorInfoCurrent() || !Guard->IsMontagePlayGuardIdentityCurrent(OriginalGuard))
	{
		FailActionPoseContract(EGGYGOMontageTaskFailureStage::Playback, OriginalSlot,
			TEXT("Original pose provider, Task/Ability/ASC/ActorInfo context or playback Guard lifecycle is no longer current."));
		return;
	}
	FString Diagnostic;
	// Poll must see even an invalidated ticket; IsValid becoming false cannot skip failure.
	const EGGYGOActionPoseContractPollResult Polled = Guard->PollActionPoseContract(OriginalTicket, Diagnostic);
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get();
	if (!Task || Task->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) || Task->bEndingTask
		|| Task->bCancellationRequested || Task->bPoseFailureRequested
		|| Task->GetState() != EGameplayTaskState::Active) { return; }
	if (!OriginalAbility.IsValid() || !OriginalAbility->IsActive() || Task->Ability != OriginalAbility.Get()
		|| Task->AbilitySystemComponent.Get() != OriginalASC.Get())
	{
		Task->EndTask();
		return;
	}
	Guard = Cast<UGGYGOMontageGuardAnimInstance>(Task->ActivatedAnimInstance.Get());
	if (!Guard || !Task->IsActivatedActorInfoCurrent() || !Guard->IsMontagePlayGuardIdentityCurrent(OriginalGuard))
	{
		Task->FailActionPoseContract(EGGYGOMontageTaskFailureStage::Playback, OriginalSlot,
			FString::Printf(TEXT("Original context changed during pose contract Poll (result=%d); %s"), int32(Polled), *Diagnostic));
		return;
	}
	if (Polled != EGGYGOActionPoseContractPollResult::Valid)
	{
		Task->FailActionPoseContract(EGGYGOMontageTaskFailureStage::Playback, OriginalSlot,
			FString::Printf(TEXT("Pose contract Poll result=%d; %s"), int32(Polled), *Diagnostic));
	}
	// No task access after failure callbacks. Never acquire a replacement ticket here.
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ExternalCancel()
{
	if (bEndingTask || bCancellationRequested || bPoseFailureRequested) { return; }
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
	bWindowFactDispatching = false;
	DispatchingWindowFact = {};
	ConsumedWindowSourceDispatches.Reset();
	ConfiguredMontageWindowDeclarations.Reset();
	// Close the subscription before any cleanup. Its captures stay stack-owned until all old
	// cleanup and Super have returned, so their destructors cannot interrupt a member-write tail.
	TSharedPtr<FNativeCallbackRegistration> DetachedNativeCallbacks = MoveTemp(NativeCallbackRegistration);
	if (UAnimInstance* OriginalAnimInstance = ActivatedAnimInstance.Get())
	{
		OriginalAnimInstance->OnPlayMontageNotifyBegin.RemoveDynamic(this, &ThisClass::OnOriginalMontageNotifyBegin);
	}
	ConfiguredMontageNotifyDeclarations.Reset();
	ActionPoseContractTicket.Reset();
	// Keep bTickingTask until native deactivation removes this task from TickingTasks.
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
	// 被别的播放替换由原生 BlendOut／Interrupted 通知；这里只看本 Task 与本实例仍在播放。
	const FAnimMontageInstance* Instance = GetTaskMontageInstance();
	return !bEndingTask && !bCancellationRequested && !bPoseFailureRequested && !bBlendingOut && IsActivatedActorInfoCurrent()
		&& OriginalPlayback.HasPlayback() && Instance && Instance->IsActive() && Instance->IsPlaying();
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::IsActivatedActorInfoCurrent() const
{
	// 播放实例属于激活时的 AnimInstance；Avatar 或 Mesh 被替换后它不再是 ASC 当前的 AnimInstance。
	const UAbilitySystemComponent* ASC = ActivatedASC.Get();
	const UGameplayAbility* OriginalAbility = ActivatedAbility.Get();
	const UAnimInstance* AnimInstance = ActivatedAnimInstance.Get();
	return ASC && OriginalAbility && AnimInstance && Ability == OriginalAbility && AbilitySystemComponent.Get() == ASC
		&& ASC->AbilityActorInfo.IsValid() && ASC->AbilityActorInfo->GetAnimInstance() == AnimInstance;
}

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::MatchesOriginalMontageCallback(
	const FGGYGOMontagePlayGuardIdentity& Original) const
{
	// The engine queues a copy of the exact instance delegate. The instance may already be gone.
	return !bEndingTask && !bCancellationRequested && !bPoseFailureRequested
		&& Original.CallId != 0 && Original.CreatedInstanceId != INDEX_NONE
		&& Original.CallId == OriginalPlayback.Guard.CallId
		&& Original.LifecycleGeneration == OriginalPlayback.Guard.LifecycleGeneration
		&& Original.CreatedInstanceId == MontageInstanceId
		&& Original.CreatedInstanceId == OriginalPlayback.Guard.CreatedInstanceId
		&& Original.OriginalAnimInstance.HasSameIndexAndSerialNumber(OriginalPlayback.Guard.OriginalAnimInstance);
}

FAnimMontageInstance* UGGYGOAbilityTask_PlayMontageAndWaitForEvent::GetTaskMontageInstance() const
{
	UGGYGOMontageGuardAnimInstance* Guard = Cast<UGGYGOMontageGuardAnimInstance>(ActivatedAnimInstance.Get());
	if (!Guard || MontageInstanceId == INDEX_NONE || MontageInstanceId != OriginalPlayback.Guard.CreatedInstanceId
		|| !Guard->IsMontagePlayGuardIdentityCurrent(OriginalPlayback.Guard))
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
		TEXT("MontageTask [%s] 启动失败：Ability [%s]，ASC [%s]，Avatar [%s]，Montage [%s]；%s；Guard=%d Stage=%d Call=%llu Instance=%d。"),
		*GetNameSafe(this), *GetNameSafe(Ability), *GetNameSafe(AbilitySystemComponent.Get()),
		*GetNameSafe(GetAvatarActor()), *GetNameSafe(MontageToPlay), Reason,
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

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FailActionPoseContract(
	EGGYGOMontageTaskFailureStage Stage, FName SlotName, const FString& Diagnostic)
{
	check(IsInGameThread());
	if (HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) || bEndingTask || bCancellationRequested
		|| bPoseFailureRequested || GetState() == EGameplayTaskState::Finished) { return; }

	FGGYGOMontageTaskFailureFact Fact;
	Fact.Montage = MontageToPlay;
	Fact.MontageInstanceId = Stage == EGGYGOMontageTaskFailureStage::Playback ? MontageInstanceId : INDEX_NONE;
	Fact.SlotName = SlotName;
	Fact.Stage = Stage;
	Fact.Diagnostic = Diagnostic;
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(this);
	const TWeakObjectPtr<UGameplayAbility> OriginalAbility(Ability);
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalASC(AbilitySystemComponent.Get());
	const FGameplayAbilitySpecHandle OriginalSpecHandle = Ability ? Ability->GetCurrentAbilitySpecHandle() : FGameplayAbilitySpecHandle();
	const FGGYGOMontagePlayGuardIdentity OriginalGuard = OriginalPlayback.Guard;
	const FGGYGOAbilityMontagePlayback OriginalHandle = OriginalPlayback;
	const TWeakObjectPtr<UAbilitySystemComponent> PlaybackASC = ActivatedASC;
	const TWeakObjectPtr<UAnimMontage> OriginalMontage(MontageToPlay);
	TSharedPtr<FNativeCallbackRegistration> NativeSnapshot = MoveTemp(NativeCallbackRegistration);
	FGGYGOMontageTaskFailureBPDelegate BlueprintSnapshot = OnFailed;
	OnFailed.Clear();
	bPoseFailureRequested = true;
	UE_LOG(LogGGYGOAbilitySystem, Error,
		TEXT("[AbilitySystem.MontageTask.Pose] Task='%s' Ability='%s' ASC='%s' Avatar='%s' Mesh='%s' Anim='%s' Montage='%s' Slot='%s' Stage=%d Generation=%llu Call=%llu Instance=%d; %s"),
		*GetPathName(), *GetPathNameSafe(OriginalAbility.Get()), *GetPathNameSafe(OriginalASC.Get()),
		*GetPathNameSafe(GetAvatarActor()), *GetPathNameSafe(ActivatedMesh.Get()),
		*GetPathNameSafe(ActivatedAnimInstance.Get()), *GetPathNameSafe(Fact.Montage.Get()),
		*Fact.SlotName.ToString(), int32(Stage), OriginalGuard.LifecycleGeneration,
		OriginalGuard.CallId, Fact.MontageInstanceId, *Fact.Diagnostic);

	// Observe only this cleanup/delivery stack. IsActive alone cannot distinguish End ->
	// reactivate of the same GA UObject; this does not issue a second activation identity.
	const TSharedRef<bool> OriginalAbilityEnded = MakeShared<bool>(false);
	FDelegateHandle EndedHandle;
	if (UGameplayAbility* Owner = OriginalAbility.Get())
	{
		EndedHandle = Owner->OnGameplayAbilityEndedWithData.AddLambda(
			[OriginalAbility, OriginalSpecHandle, OriginalAbilityEnded](const FAbilityEndedData& Data)
			{
				if (Data.AbilityThatEnded.Get() == OriginalAbility.Get() && Data.AbilitySpecHandle == OriginalSpecHandle)
				{
					*OriginalAbilityEnded = true;
				}
			});
	}
	ON_SCOPE_EXIT
	{
		if (UGameplayAbility* Owner = OriginalAbility.Get())
		{
			Owner->OnGameplayAbilityEndedWithData.Remove(EndedHandle);
		}
	};

	RequestInFlightMontageStop();
	ReleaseRootMotionScaleLease();
	if (Stage == EGGYGOMontageTaskFailureStage::Playback)
	{
		StopOriginalMontageInstance(OriginalGuard, OriginalHandle, PlaybackASC, OriginalMontage, OriginalTask);
	}
	if (UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get())
	{
		if (!Task->bEndingTask && Task->GetState() != EGameplayTaskState::Finished) { Task->EndTask(); }
	}

	const auto Recheck = [&]() -> UGGYGOAbilityTask_PlayMontageAndWaitForEvent*
	{
		// Native OnDestroy clears Ability and marks the Task as garbage. This synchronous
		// historical envelope reads the surviving original object, never a live successor.
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get(true);
		return Task && !Task->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) && Task->bPoseFailureRequested
			&& !*OriginalAbilityEnded && OriginalAbility.IsValid() && OriginalAbility->IsActive()
			&& OriginalAbility.HasSameIndexAndSerialNumber(Task->ActivatedAbility)
			&& OriginalASC.IsValid() && OriginalASC.HasSameIndexAndSerialNumber(Task->ActivatedASC)
			? Task : nullptr;
	};
	// Historical failure is delivered after EndTask. Its original GA receiver authenticates
	// activation/step/resource; requiring a live montage or an Active Task would swallow it.
	if (Recheck() && NativeSnapshot.IsValid()) { NativeSnapshot->Callbacks.OnFailed.ExecuteIfBound(Fact); }
	NativeSnapshot.Reset(); // Capture destruction may end/reactivate the GA too.
	if (UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = Recheck())
	{
		// 原生 OnDestroy 已清空 Task->Ability，这里只核对原 AnimInstance 仍是 ASC 当前的 AnimInstance。
		if (OriginalASC->AbilityActorInfo.IsValid() && Task->ActivatedAnimInstance.IsValid()
			&& OriginalASC->AbilityActorInfo->GetAnimInstance() == Task->ActivatedAnimInstance.Get())
		{
			BlueprintSnapshot.Broadcast(Fact);
		}
	}
	// No task or old resource access after BP; scope exit only removes our exact temporary observer.
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
	return StopOriginalMontageInstance(OriginalPlayback.Guard, OriginalPlayback, ActivatedASC,
		TWeakObjectPtr<UAnimMontage>(MontageToPlay.Get()), TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent>(this));
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::OnAbilityCancelled()
{
	if (bEndingTask || bCancellationRequested || bPoseFailureRequested) { return; }
	bCancellationRequested = true;
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(this);
	RequestInFlightMontageStop();
	ReleaseRootMotionScaleLease();
	StopPlayingMontage();
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get();
	if (Task && !Task->bEndingTask && Task->GetState() != EGameplayTaskState::Finished)
	{
		Task->DispatchOriginalCallback(ENativeCallback::Cancelled, FGameplayTag(), FGameplayEventData(),
			/*bFromOwnerCancellation=*/true);
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
	if (bEndingTask || bCancellationRequested || bPoseFailureRequested || bBlendingOut || Montage != MontageToPlay) { return; }
	bBlendingOut = true;
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(this);
	const FGGYGOAbilityMontagePlayback Playback = OriginalPlayback;
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalASC = ActivatedASC;
	ReleaseRootMotionScaleLease();
	// 混出开始即归还本次播放的 GAS 归属；已被替换时 ClearPlayback 不触碰后继。
	if (UGGYGOAbilitySystemComponent* ASC = Cast<UGGYGOAbilitySystemComponent>(OriginalASC.Get()))
	{
		ASC->ClearPlayback(Playback);
	}
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get();
	if (!Task || !Task->MatchesOriginalMontageCallback(Playback.Guard)) { return; }
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
	if (bEndingTask || bCancellationRequested || bPoseFailureRequested || Montage != MontageToPlay) { return; }
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

bool UGGYGOAbilityTask_PlayMontageAndWaitForEvent::IsOriginalMontageWindowFact(
	const FGGYGOMontageWindowFact& Fact) const
{
	if (!bWindowFactDispatching || Fact.Issuer.Get() != this || Fact.DispatchSerial == 0
		|| Fact.DispatchSerial != DispatchingWindowFact.DispatchSerial
		|| Fact.Montage != DispatchingWindowFact.Montage || Fact.Mesh != DispatchingWindowFact.Mesh
		|| Fact.MontageInstanceId != DispatchingWindowFact.MontageInstanceId
		|| Fact.Phase != DispatchingWindowFact.Phase
		|| Fact.DispatchWorldTimeSeconds != DispatchingWindowFact.DispatchWorldTimeSeconds
		|| !Fact.Declaration.HasSameWindow(DispatchingWindowFact.Declaration)
		|| !IsOriginalMontageWindowDeclarationCurrent(Fact.Declaration)) { return false; }
	return Fact.Phase == EGGYGOMontageWindowPhase::Begin
		? IsNotifyValid() : CanDispatchOriginalInstanceFact(OriginalPlayback.Guard);
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::DispatchOriginalMontageWindowFact(
	FGameplayTag EventTag, const FGameplayEventData& Payload)
{
	FGGYGOMontageWindowSourceData Source;
	if (!UGGYGOAnimNotifyState_GameplayEventWindow::TryReadWindowDispatch(&Payload, Source)
		|| !Source.DispatchIdentity.IsValid()
		|| Source.EventTag != EventTag || !Source.bHasMontageInstanceContext
		|| Source.MontageInstanceId != MontageInstanceId || MontageInstanceId == INDEX_NONE
		|| Source.Animation.Get() != MontageToPlay.Get() || Source.Mesh != ActivatedMesh
		|| Source.AnimInstance != ActivatedAnimInstance || !Source.Notify.IsValid()) { return; }
	ConsumedWindowSourceDispatches.RemoveAll([](const TWeakPtr<const void>& Identity) { return !Identity.IsValid(); });
	if (ConsumedWindowSourceDispatches.ContainsByPredicate([&Source](const TWeakPtr<const void>& Identity)
		{ return Identity.Pin() == Source.DispatchIdentity; })) { return; }
	if ((Source.Phase != EGGYGOMontageWindowPhase::Begin && Source.Phase != EGGYGOMontageWindowPhase::End)
		|| (Source.Phase == EGGYGOMontageWindowPhase::Begin
			? !IsNotifyValid() : !CanDispatchOriginalInstanceFact(OriginalPlayback.Guard))) { return; }
	const FGGYGOMontageWindowDeclaration* Match = nullptr;
	for (const auto& Declaration : ConfiguredMontageWindowDeclarations)
	{
		if (Declaration.NativeNotify.Get() != Source.Notify.Get()
			|| Declaration.BeginSeconds != Source.BeginSeconds || Declaration.EndSeconds != Source.EndSeconds
			|| Declaration.BeginTriggerSeconds != Source.BeginTriggerSeconds
			|| Declaration.EndTriggerSeconds != Source.EndTriggerSeconds
			|| Declaration.bQueued != Source.bQueued
			|| Declaration.bNoMergeOnConcurrentPlay != Source.bNoMergeOnConcurrentPlay
			|| (Source.Phase == EGGYGOMontageWindowPhase::Begin
				? Declaration.BeginEventTag : Declaration.EndEventTag) != EventTag) { continue; }
		if (Match)
		{
			FailAndEndTask(TEXT("原窗口来源与多个相同作者条目相符，不能猜测声明。"));
			return;
		}
		Match = &Declaration;
	}
	if (!Match) { return; }
	if (!IsOriginalMontageWindowDeclarationCurrent(*Match))
	{
		FailAndEndTask(TEXT("已配置原窗口的作者来源发生变化。"));
		return;
	}
	UWorld* World = Source.Mesh->GetWorld();
	if (!World || World->bIsTearingDown || !FMath::IsFinite(World->GetTimeSeconds())) { return; }
	if (LastWindowDispatchSerial == MAX_uint64)
	{
		FailAndEndTask(TEXT("原窗口同步派发身份已耗尽。"));
		return;
	}
	FGGYGOMontageWindowFact Fact;
	Fact.Montage = MontageToPlay;
	Fact.Mesh = Source.Mesh.Get();
	Fact.MontageInstanceId = MontageInstanceId;
	Fact.Declaration = *Match;
	Fact.Phase = Source.Phase;
	Fact.DispatchWorldTimeSeconds = World->GetTimeSeconds();
	Fact.Issuer = this;
	Fact.DispatchSerial = ++LastWindowDispatchSerial;
	ConsumedWindowSourceDispatches.Add(Source.DispatchIdentity);
	const FGGYGOMontageWindowFact PreviousFact = DispatchingWindowFact;
	const bool bPreviousDispatching = bWindowFactDispatching;
	DispatchingWindowFact = Fact;
	bWindowFactDispatching = true;
	const TWeakObjectPtr<ThisClass> OriginalTask(this);
	const auto Registration = NativeCallbackRegistration;
	if (Registration.IsValid() && Registration->Callbacks.WindowReceived.IsBound())
	{
		Registration->Callbacks.WindowReceived.Execute(Fact);
	}
	ThisClass* Task = OriginalTask.Get();
	if (Task && Task->NativeCallbackRegistration == Registration && Task->IsOriginalMontageWindowFact(Fact))
	{
		Task->WindowReceived.Broadcast(Fact);
	}
	Task = OriginalTask.Get();
	if (Task && Task->bWindowFactDispatching && Task->DispatchingWindowFact.DispatchSerial == Fact.DispatchSerial)
	{
		// 嵌套派发只归还本帧的槽位；结束或后继改写后不恢复旧值。
		Task->DispatchingWindowFact = PreviousFact;
		Task->bWindowFactDispatching = bPreviousDispatching;
		if (!bPreviousDispatching || !Task->IsOriginalMontageWindowFact(PreviousFact))
		{
			Task->bWindowFactDispatching = false;
			Task->DispatchingWindowFact = {};
		}
	}
}

void UGGYGOAbilityTask_PlayMontageAndWaitForEvent::OnGameplayEvent(FGameplayTag EventTag, const FGameplayEventData* Payload)
{
	if (!Payload) { return; }
	const TWeakObjectPtr<ThisClass> OriginalTask(this);
	DispatchOriginalMontageWindowFact(EventTag, *Payload);
	ThisClass* Task = OriginalTask.Get();
	if (!Task) { return; }
	if (!Task->ShouldBroadcastAbilityTaskDelegates() || !Task->IsNotifyValid())
	{
		return;
	}
	if (const UAnimMontage* Source = Cast<UAnimMontage>(Payload->OptionalObject))
	{
		if (Source != Task->MontageToPlay) { return; }
		// GameplayEventWindow 把本地播放实例 ID+1 放入 Magnitude；0 表示无上下文。
		if (Payload->EventMagnitude > 0.0f && Task->MontageInstanceId != INDEX_NONE
			&& FMath::RoundToInt(Payload->EventMagnitude) - 1 != Task->MontageInstanceId) { return; }
	}

	// 拷一份负载再广播：原始指针指向 ASC 内部的临时对象，
	// 广播过程中若有人结束了任务，那块内存就失效了。
	FGameplayEventData TempData = *Payload;
	TempData.EventTag = EventTag;

	Task->DispatchOriginalCallback(ENativeCallback::EventReceived, EventTag, MoveTemp(TempData));
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

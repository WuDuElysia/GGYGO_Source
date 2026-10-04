/** @file GGYGOAbilityTask_WaitComboInput.cpp */
#include "AbilitySystem/Tasks/GGYGOAbilityTask_WaitComboInput.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAbilityTask_WaitComboInput)

struct UGGYGOAbilityTask_WaitComboInput::FNativeCallbackRegistration
{
	explicit FNativeCallbackRegistration(FGGYGOComboInputNativeDelegate&& InCallback)
		: Handle(FDelegateHandle::GenerateNewHandle), Callback(MoveTemp(InCallback)) {}

	const FDelegateHandle Handle;
	const FGGYGOComboInputNativeDelegate Callback;
};

struct UGGYGOAbilityTask_WaitComboInput::FOriginalInputSubscription
{
	FOriginalInputSubscription(UAbilitySystemComponent* InASC, UGameplayAbility* InAbility,
		FGameplayAbilitySpecHandle InSpec, FPredictionKey InKey, bool bInRemote, bool bInPredicting,
		FDelegateHandle InHandle)
		: ASC(InASC), OwningAbility(InAbility), Spec(InSpec), PredictionKey(InKey),
		bRemote(bInRemote), bPredicting(bInPredicting), Handle(InHandle) {}

	// This immutable resource locates one task subscription, never a GA activation identity.
	const TWeakObjectPtr<UAbilitySystemComponent> ASC;
	const TWeakObjectPtr<UGameplayAbility> OwningAbility;
	const FGameplayAbilitySpecHandle Spec;
	const FPredictionKey PredictionKey;
	const bool bRemote;
	const bool bPredicting;
	const FDelegateHandle Handle;
};

UGGYGOAbilityTask_WaitComboInput* UGGYGOAbilityTask_WaitComboInput::WaitComboInput(UGameplayAbility* OwningAbility)
{
	return NewAbilityTask<UGGYGOAbilityTask_WaitComboInput>(OwningAbility);
}

FDelegateHandle UGGYGOAbilityTask_WaitComboInput::RegisterNativeCallback(FGGYGOComboInputNativeDelegate Callback)
{
	check(IsInGameThread());
	const TCHAR* Reason = nullptr;
	if (HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) || bActivationStarted || bEndingTask
		|| GetState() != EGameplayTaskState::AwaitingActivation)
	{
		Reason = TEXT("Task is not in its original pre-Ready AwaitingActivation state");
	}
	else if (NativeCallbackRegistration.IsValid()) { Reason = TEXT("a native callback is already registered"); }
	else if (!Callback.IsBound()) { Reason = TEXT("the native callback is not bound"); }
	if (Reason)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("WaitComboInput [%s] native registration rejected: Ability [%s]; %s."),
			*GetPathName(), *GetNameSafe(Ability), Reason);
		return {};
	}

	const TWeakObjectPtr<UGGYGOAbilityTask_WaitComboInput> OriginalTask(this);
	const FString OriginalPath = GetPathName();
	TSharedPtr<FNativeCallbackRegistration> Registration = MakeShared<FNativeCallbackRegistration>(MoveTemp(Callback));
	// Moving delegate captures may run external code; install only after it has returned.
	UGGYGOAbilityTask_WaitComboInput* Task = OriginalTask.Get();
	if (!Task || Task->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) || Task->bActivationStarted
		|| Task->bEndingTask || Task->GetState() != EGameplayTaskState::AwaitingActivation
		|| Task->NativeCallbackRegistration.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("WaitComboInput [%s] native registration rejected after callback construction: original pre-Ready scope changed."),
			*OriginalPath);
		return {};
	}
	const FDelegateHandle Handle = Registration->Handle;
	Task->NativeCallbackRegistration = MoveTemp(Registration);
	return Handle; // No task writes during subsequent destruction of argument captures.
}

bool UGGYGOAbilityTask_WaitComboInput::UnregisterNativeCallback(FDelegateHandle Registration)
{
	check(IsInGameThread());
	if (!Registration.IsValid() || !NativeCallbackRegistration.IsValid()
		|| NativeCallbackRegistration->Handle != Registration) { return false; }
	TSharedPtr<FNativeCallbackRegistration> Detached = MoveTemp(NativeCallbackRegistration);
	Detached.Reset(); // Capture destruction may install a successor; no task writes follow.
	return true;
}

void UGGYGOAbilityTask_WaitComboInput::Activate()
{
	if (bEndingTask || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| GetState() == EGameplayTaskState::Finished) { return; }
	if (bActivationStarted)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("WaitComboInput [%s] rejected repeated Activate: the original subscription cannot be reused."), *GetPathName());
		return;
	}
	bActivationStarted = true;
	const TWeakObjectPtr<UGGYGOAbilityTask_WaitComboInput> OriginalTask(this);
	FSimpleMulticastDelegate::FDelegate InputCallback =
		FSimpleMulticastDelegate::FDelegate::CreateUObject(this, &ThisClass::HandlePress);
	// Capture every bucket/mode value once, before Super or synchronous GAS callbacks.
	const TSharedPtr<FOriginalInputSubscription> Subscription = MakeShared<FOriginalInputSubscription>(
		AbilitySystemComponent.Get(), Ability, GetAbilitySpecHandle(), GetActivationPredictionKey(),
		IsForRemoteClient(), IsPredictingClient(), InputCallback.GetHandle());
	if (!Subscription->ASC.IsValid() || !Subscription->OwningAbility.IsValid()
		|| !Subscription->Spec.IsValid() || !Subscription->Handle.IsValid())
	{
		RejectAndEndTask(TEXT("missing ASC/owning ability or invalid original Spec/delegate handle"), Subscription);
		return;
	}
	// A zero prediction key is legal: it is an event bucket locator, not an identity proof.
	Super::Activate();
	UGGYGOAbilityTask_WaitComboInput* Task = OriginalTask.Get();
	if (!Task || Task->bEndingTask || Task->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| Task->GetState() == EGameplayTaskState::Finished) { return; }
	if (!Subscription->ASC.IsValid() || !Subscription->OwningAbility.IsValid()
		|| !Subscription->ASC.HasSameIndexAndSerialNumber(Task->AbilitySystemComponent)
		|| !Subscription->OwningAbility.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGameplayAbility>(Task->Ability))
		|| !Task->ShouldBroadcastAbilityTaskDelegates())
	{
		Task->RejectAndEndTask(TEXT("original input source changed or owning ability ended before subscription"), Subscription);
		return;
	}
	Task->OriginalInputSubscription = Subscription;
	UAbilitySystemComponent* ASC = Subscription->ASC.Get();
	// This GAS accessor/Add does not invoke callbacks. The delegate's exact handle was captured above.
	ASC->AbilityReplicatedEventDelegate(EAbilityGenericReplicatedEvent::InputPressed,
		Subscription->Spec, Subscription->PredictionKey).Add(MoveTemp(InputCallback));
	// 不测试 Spec.InputPressed，首次激活的按下不算下一段请求。
	if (Subscription->bRemote)
	{
		const bool bReplayed = ASC->CallReplicatedEventDelegateIfSet(
			EAbilityGenericReplicatedEvent::InputPressed, Subscription->Spec, Subscription->PredictionKey);
		Task = RecheckOriginalSubscription(OriginalTask, Subscription);
		if (Task && !bReplayed) { Task->SetWaitingOnRemotePlayerData(); }
		// Waiting may call into the ability. No old task access follows.
	}
}

UGGYGOAbilityTask_WaitComboInput* UGGYGOAbilityTask_WaitComboInput::RecheckOriginalSubscription(
	const TWeakObjectPtr<UGGYGOAbilityTask_WaitComboInput>& OriginalTask,
	const TSharedPtr<FOriginalInputSubscription>& Subscription)
{
	UGGYGOAbilityTask_WaitComboInput* Task = OriginalTask.Get();
	if (!Task || Task->bEndingTask || Task->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| Task->GetState() == EGameplayTaskState::Finished) { return nullptr; }
	if (!Subscription.IsValid() || Task->OriginalInputSubscription != Subscription
		|| !Subscription->ASC.IsValid() || !Subscription->OwningAbility.IsValid()
		|| !Subscription->Spec.IsValid() || !Subscription->Handle.IsValid()
		|| !Subscription->ASC.HasSameIndexAndSerialNumber(Task->AbilitySystemComponent)
		|| !Subscription->OwningAbility.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGameplayAbility>(Task->Ability)))
	{
		Task->RejectAndEndTask(TEXT("original input subscription/ASC/owner is invalid or changed"), Subscription);
		return nullptr;
	}
	// GAS owns activity; a caller's native closure owns the original activation check.
	return Task->ShouldBroadcastAbilityTaskDelegates() ? Task : nullptr;
}

void UGGYGOAbilityTask_WaitComboInput::RejectAndEndTask(
	const TCHAR* Reason, const TSharedPtr<FOriginalInputSubscription>& Subscription)
{
	if (bEndingTask || GetState() == EGameplayTaskState::Finished) { return; }
	UE_LOG(LogGGYGOAbilitySystem, Error,
		TEXT("WaitComboInput [%s] rejected: original ASC [%s], Ability [%s], Spec [%s], PredictionKey [%s]; %s."),
		*GetPathName(), *GetNameSafe(Subscription.IsValid() ? Subscription->ASC.Get() : nullptr),
		*GetNameSafe(Subscription.IsValid() ? Subscription->OwningAbility.Get() : nullptr),
		Subscription.IsValid() ? *Subscription->Spec.ToString() : TEXT("<uncaptured>"),
		Subscription.IsValid() ? *Subscription->PredictionKey.ToString() : TEXT("<uncaptured>"), Reason);
	EndTask(); // No success notification or task access after own cleanup/capture destruction.
}

void UGGYGOAbilityTask_WaitComboInput::HandlePress()
{
	const TWeakObjectPtr<UGGYGOAbilityTask_WaitComboInput> OriginalTask(this);
	const TSharedPtr<FOriginalInputSubscription> Subscription = OriginalInputSubscription;
	UGGYGOAbilityTask_WaitComboInput* Task = RecheckOriginalSubscription(OriginalTask, Subscription);
	if (!Task) { return; }
	UAbilitySystemComponent* ASC = Subscription->ASC.Get();
	const FString OriginalPath = Task->GetPathName();
	FScopedPredictionWindow Prediction(ASC, Subscription->bPredicting);
	Task = RecheckOriginalSubscription(OriginalTask, Subscription);
	if (!Task) { return; }
	int32 RequestStep = Task->SourceStep; // SourceStep remains mutable for later combo steps.
	int32 RequestId = 0;
	if (Subscription->bRemote)
	{
		const FAbilityReplicatedData Data = ASC->GetReplicatedDataOfGenericReplicatedEvent(
			EAbilityGenericReplicatedEvent::InputPressed, Subscription->Spec, Subscription->PredictionKey);
		// GAS 通用事件提供 VectorPayload；这里只传精确的小整数 X=段序、Y=请求序号。
		const FVector& Payload = Data.VectorPayload;
		if (!FMath::IsFinite(Payload.X) || !FMath::IsFinite(Payload.Y)
			|| Payload.X < 0.0 || Payload.X > 65535.0 || Payload.Y < 1.0 || Payload.Y > 65535.0
			|| Payload.X != FMath::FloorToDouble(Payload.X) || Payload.Y != FMath::FloorToDouble(Payload.Y))
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("WaitComboInput [%s] rejected InputPressed payload: original ASC [%s], Spec [%s], PredictionKey [%s], X=%g Y=%g; expected integer X in [0,65535], Y in [1,65535]."),
				*OriginalPath, *GetNameSafe(ASC), *Subscription->Spec.ToString(), *Subscription->PredictionKey.ToString(),
				static_cast<double>(Payload.X), static_cast<double>(Payload.Y));
			ASC->ConsumeGenericReplicatedEvent(EAbilityGenericReplicatedEvent::InputPressed,
				Subscription->Spec, Subscription->PredictionKey);
			return;
		}
		RequestStep = FMath::RoundToInt(Data.VectorPayload.X);
		RequestId = FMath::RoundToInt(Data.VectorPayload.Y);
	}
	else
	{
		RequestId = ++Task->LocalRequestId;
		if (Subscription->bPredicting)
		{
			ASC->ServerSetReplicatedEventWithPayload(EAbilityGenericReplicatedEvent::InputPressed,
				Subscription->Spec, Subscription->PredictionKey, ASC->ScopedPredictionKey,
				FVector(RequestStep, RequestId, 0.0));
		}
	}
	Task = RecheckOriginalSubscription(OriginalTask, Subscription);
	if (!Task) { return; }
	// 先消费原桶，再通知；旧 Task 结束后的解绑不消费，避免清掉后继请求。
	ASC->ConsumeGenericReplicatedEvent(EAbilityGenericReplicatedEvent::InputPressed,
		Subscription->Spec, Subscription->PredictionKey);
	Task = RecheckOriginalSubscription(OriginalTask, Subscription);
	if (!Task) { return; }
	TSharedPtr<FNativeCallbackRegistration> NativeSnapshot = Task->NativeCallbackRegistration;
	if (NativeSnapshot.IsValid()) { NativeSnapshot->Callback.ExecuteIfBound(RequestStep, RequestId); }
	NativeSnapshot.Reset(); // Captured destructors are external code too; wait for their return.
	Task = RecheckOriginalSubscription(OriginalTask, Subscription);
	if (!Task) { return; }
	Task->OnPress.Broadcast(RequestStep, RequestId);
	// Preserve the original prediction window through notification; no task writes after it unwinds.
}

void UGGYGOAbilityTask_WaitComboInput::OnDestroy(bool AbilityEnded)
{
	if (bEndingTask) { return; }
	bEndingTask = true;
	TSharedPtr<FNativeCallbackRegistration> DetachedNative = MoveTemp(NativeCallbackRegistration);
	TSharedPtr<FOriginalInputSubscription> DetachedSubscription = MoveTemp(OriginalInputSubscription);
	if (DetachedSubscription.IsValid())
	{
		if (UAbilitySystemComponent* ASC = DetachedSubscription->ASC.Get())
		{
			ASC->AbilityReplicatedEventDelegate(EAbilityGenericReplicatedEvent::InputPressed,
				DetachedSubscription->Spec, DetachedSubscription->PredictionKey).Remove(DetachedSubscription->Handle);
		}
	}
	Super::OnDestroy(AbilityEnded);
	DetachedNative.Reset(); // Own cleanup/Super returned; no task access after capture destruction.
}

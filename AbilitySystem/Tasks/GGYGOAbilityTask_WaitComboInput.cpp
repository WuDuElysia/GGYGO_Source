/** @file GGYGOAbilityTask_WaitComboInput.cpp @brief 原步骤订阅、真入场一次提交及 owned 数据派发。 */
#include "AbilitySystem/Tasks/GGYGOAbilityTask_WaitComboInput.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "CoreGlobals.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Serialization/Archive.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAbilityTask_WaitComboInput)

bool FGGYGOAbilityStepInputTargetData::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	(void)Map;
	if (Ar.IsLoading()) OwnedRequest = {};
	uint8 KindValue = static_cast<uint8>(Kind);
	uint8 StateValue = static_cast<uint8>(StepFact.InputState);
	Ar << KindValue << SourceStep << RequestId;
	Ar << StepFact.SourceSessionSerial << StepFact.OriginalEdgeEvaluationSerial << StepFact.StepEvaluationSerial;
	Ar << StateValue << StepFact.ActionValue.X << StepFact.ActionValue.Y << StepFact.ControlYaw;
	if (Ar.IsLoading())
	{
		Kind = static_cast<EGGYGOAbilityStepInputKind>(KindValue);
		StepFact.InputState = static_cast<EGGYGOMovementInputRequestQueryResult>(StateValue);
	}
	bOutSuccess = !Ar.IsError() && HasValidFields();
	return bOutSuccess;
}

bool FGGYGOAbilityStepInputTargetData::HasValidFields() const
{
	const bool bInitial = Kind == EGGYGOAbilityStepInputKind::Initial;
	if (!(bInitial ? SourceStep == INDEX_NONE && RequestId == 0
		: Kind == EGGYGOAbilityStepInputKind::Continue && SourceStep >= 0 && SourceStep <= 65535
			&& RequestId > 0 && RequestId <= 65535)) return false;
	const bool bHeld = StepFact.InputState == EGGYGOMovementInputRequestQueryResult::Held;
	// NotHeld 与无物理证明都不带方向，段首按当前朝向。
	const bool bNoDirection = StepFact.InputState == EGGYGOMovementInputRequestQueryResult::NotHeld
		|| StepFact.InputState == EGGYGOMovementInputRequestQueryResult::AwaitingPhysicalProof;
	return StepFact.SourceSessionSerial != 0 && StepFact.OriginalEdgeEvaluationSerial != 0
		&& StepFact.StepEvaluationSerial >= StepFact.OriginalEdgeEvaluationSerial && (bHeld || bNoDirection)
		&& FMath::IsFinite(StepFact.ActionValue.X) && FMath::IsFinite(StepFact.ActionValue.Y)
		&& FMath::IsFinite(StepFact.ControlYaw)
		&& (!bNoDirection || (StepFact.ActionValue.X == 0.0 && StepFact.ActionValue.Y == 0.0));
}

struct UGGYGOAbilityTask_WaitComboInput::FNativeCallbackRegistration
{
	FNativeCallbackRegistration(FGGYGOComboInputNativeDelegate&& InCallback, FSimpleDelegate&& InUnavailable)
		: Handle(FDelegateHandle::GenerateNewHandle), Callback(MoveTemp(InCallback)), InitialUnavailable(MoveTemp(InUnavailable)) {}

	const FDelegateHandle Handle;
	const FGGYGOComboInputNativeDelegate Callback;
	const FSimpleDelegate InitialUnavailable;
};

struct UGGYGOAbilityTask_WaitComboInput::FOriginalInputSubscription
{
	FOriginalInputSubscription(UAbilitySystemComponent* InASC, UGameplayAbility* InAbility,
		FGameplayAbilitySpecHandle InSpec, FPredictionKey InKey, bool bInRemote, bool bInPredicting,
		FDelegateHandle InHandle, FDelegateHandle InCancelHandle, const FGGYGOAbilityActivationHandle& InActivation)
		: ASC(InASC), OwningAbility(InAbility), Spec(InSpec), PredictionKey(InKey),
		bRemote(bInRemote), bPredicting(bInPredicting), Handle(InHandle), CancellationHandle(InCancelHandle), Activation(InActivation) {}

	// 桶定位与已签发原激活分别保存；Task 不签发或更新激活身份。
	const TWeakObjectPtr<UAbilitySystemComponent> ASC;
	const TWeakObjectPtr<UGameplayAbility> OwningAbility;
	const FGameplayAbilitySpecHandle Spec;
	const FPredictionKey PredictionKey;
	const bool bRemote;
	const bool bPredicting;
	const FDelegateHandle Handle;
	const FDelegateHandle CancellationHandle;
	const FGGYGOAbilityActivationHandle Activation;
};

UGGYGOAbilityTask_WaitComboInput* UGGYGOAbilityTask_WaitComboInput::WaitComboInput(UGameplayAbility* OwningAbility)
{
	UGGYGOGameplayAbility* ProjectAbility = Cast<UGGYGOGameplayAbility>(OwningAbility);
	UGGYGOAbilitySystemComponent* ASC = ProjectAbility ? Cast<UGGYGOAbilitySystemComponent>(ProjectAbility->GetAbilitySystemComponentFromActorInfo()) : nullptr;
	if (!ASC) return nullptr;
	FString Error;
	const auto Route = ASC->CaptureInitialInputRoute(ProjectAbility, ProjectAbility->CaptureCurrentActivation(), Error);
	return WaitComboInput(OwningAbility, Route);
}

UGGYGOAbilityTask_WaitComboInput* UGGYGOAbilityTask_WaitComboInput::WaitComboInput(
	UGameplayAbility* OwningAbility, const FGGYGOAbilityInitialInputRoute& InitialRoute)
{
	const TWeakObjectPtr<UGGYGOGameplayAbility> OriginalAbility(Cast<UGGYGOGameplayAbility>(OwningAbility));
	const auto Original = OriginalAbility.IsValid() ? OriginalAbility->CaptureCurrentActivation() : FGGYGOAbilityActivationHandle{};
	if (!Original.HasActivation()) return nullptr;
	UGGYGOAbilitySystemComponent* ASC = Cast<UGGYGOAbilitySystemComponent>(OriginalAbility->GetAbilitySystemComponentFromActorInfo());
	FString Error;
	const auto Canonical = ASC ? ASC->CaptureInitialInputRoute(OriginalAbility.Get(), Original, Error) : FGGYGOAbilityInitialInputRoute{};
	if (!InitialRoute.HasSameRoute(Canonical)) return nullptr;
	auto* Task = NewAbilityTask<UGGYGOAbilityTask_WaitComboInput>(OwningAbility);
	if (!Task) return nullptr;
	if (!OriginalAbility.IsValid() || !OriginalAbility->CaptureCurrentActivation().HasSameActivation(Original))
	{ Task->EndTask(); return nullptr; }
	Task->OriginalActivationIdentity = Original.GetResourceIdentity();
	Task->InitialInputRoute = InitialRoute;
	return Task;
}

FDelegateHandle UGGYGOAbilityTask_WaitComboInput::RegisterNativeCallback(
	FGGYGOComboInputNativeDelegate Callback, FSimpleDelegate InitialUnavailable)
{
	check(IsInGameThread());
	const TCHAR* Reason = nullptr;
	if (HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) || bActivationStarted || bEndingTask
		|| GetState() != EGameplayTaskState::AwaitingActivation)
	{
		Reason = TEXT("Task is not in its original pre-Ready AwaitingActivation state");
	}
	else if (NativeCallbackRegistration.IsValid()) { Reason = TEXT("a native callback is already registered"); }
	else if (!Callback.IsBound() || !InitialUnavailable.IsBound()) { Reason = TEXT("owned 请求及 Initial 不可提供回调均须绑定"); }
	if (Reason)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("WaitComboInput [%s] native registration rejected: Ability [%s]; %s."),
			*GetPathName(), *GetNameSafe(Ability), Reason);
		return {};
	}

	const TWeakObjectPtr<UGGYGOAbilityTask_WaitComboInput> OriginalTask(this);
	const FString OriginalPath = GetPathName();
	TSharedPtr<FNativeCallbackRegistration> Registration = MakeShared<FNativeCallbackRegistration>(MoveTemp(Callback), MoveTemp(InitialUnavailable));
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
	if (bEndingTask || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) || GetState() == EGameplayTaskState::Finished) return;
	if (bActivationStarted)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("WaitComboInput [%s] 重复 Ready 被拒绝。"), *GetPathName());
		return;
	}
	bActivationStarted = true;
	const TWeakObjectPtr<UGGYGOAbilityTask_WaitComboInput> OriginalTask(this);
	const bool bRemote = IsForRemoteClient();
	const UGGYGOGameplayAbility* OriginalAbility = Cast<UGGYGOGameplayAbility>(Ability);
	const auto OriginalActivation = OriginalAbility ? OriginalAbility->CaptureCurrentActivation() : FGGYGOAbilityActivationHandle{};
	if (!OriginalActivation.HasActivation() || OriginalActivation.GetResourceIdentity() != OriginalActivationIdentity)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("WaitComboInput [%s] Ready 不属于创建时原激活。"), *GetPathName());
		EndTask();
		return;
	}
	FSimpleMulticastDelegate::FDelegate LocalCallback;
	FSimpleMulticastDelegate::FDelegate CancelCallback;
	FAbilityTargetDataSetDelegate::FDelegate RemoteCallback;
	if (bRemote)
	{
		RemoteCallback = FAbilityTargetDataSetDelegate::FDelegate::CreateUObject(this, &ThisClass::HandleTargetData);
		CancelCallback = FSimpleMulticastDelegate::FDelegate::CreateUObject(this, &ThisClass::HandleInitialUnavailable);
	}
	else LocalCallback = FSimpleMulticastDelegate::FDelegate::CreateUObject(this, &ThisClass::HandlePress);
	const auto Subscription = MakeShared<FOriginalInputSubscription>(
		AbilitySystemComponent.Get(), Ability, GetAbilitySpecHandle(), GetActivationPredictionKey(),
		bRemote, IsPredictingClient(), bRemote ? RemoteCallback.GetHandle() : LocalCallback.GetHandle(),
		CancelCallback.GetHandle(), OriginalActivation);
	if (!Subscription->ASC.IsValid() || !Subscription->OwningAbility.IsValid()
		|| !Subscription->Spec.IsValid() || !Subscription->Handle.IsValid() || !Subscription->Activation.HasActivation()
		|| (bRemote && !Subscription->CancellationHandle.IsValid()))
	{ RejectAndEndTask(TEXT("原 ASC/Ability/Spec/订阅不存在"), Subscription); return; }
	Super::Activate();
	UGGYGOAbilityTask_WaitComboInput* Task = OriginalTask.Get();
	if (!Task || Task->bEndingTask || Task->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| Task->GetState() == EGameplayTaskState::Finished) return;
	if (!Subscription->ASC.IsValid() || !Subscription->OwningAbility.IsValid()
		|| !Subscription->ASC.HasSameIndexAndSerialNumber(Task->AbilitySystemComponent)
		|| !Subscription->OwningAbility.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGameplayAbility>(Task->Ability))
		|| !OriginalAbility->CaptureCurrentActivation().HasSameActivation(Subscription->Activation)
		|| !Task->ShouldBroadcastAbilityTaskDelegates())
	{ Task->RejectAndEndTask(TEXT("Ready 返回后原来源失效"), Subscription); return; }
	Task->OriginalInputSubscription = Subscription;
	UAbilitySystemComponent* ASC = Subscription->ASC.Get();
	if (bRemote)
	{
		ASC->AbilityTargetDataSetDelegate(Subscription->Spec, Subscription->PredictionKey).Add(MoveTemp(RemoteCallback));
		ASC->AbilityTargetDataCancelledDelegate(Subscription->Spec, Subscription->PredictionKey).Add(MoveTemp(CancelCallback));
		ASC->CallReplicatedTargetDataDelegatesIfSet(Subscription->Spec, Subscription->PredictionKey);
		Task = RecheckOriginalSubscription(OriginalTask, Subscription);
		if (Task && !Task->bInitialReceived) Task->SetWaitingOnRemotePlayerData();
		return;
	}
	ASC->AbilityReplicatedEventDelegate(EAbilityGenericReplicatedEvent::InputPressed,
		Subscription->Spec, Subscription->PredictionKey).Add(MoveTemp(LocalCallback));
	// 首次请求只取原受控激活来源，不读 Spec.InputPressed 或合成续击。
	UGGYGOAbilitySystemComponent* ProjectASC = Cast<UGGYGOAbilitySystemComponent>(ASC);
	FString Error;
	const auto Initial = ProjectASC ? ProjectASC->CaptureStepInputRequest(Task,
		Cast<UGGYGOGameplayAbility>(Subscription->OwningAbility.Get()),
		EGGYGOAbilityStepInputKind::Initial, INDEX_NONE, 0, Error) : FGGYGOAbilityStepInputRequest{};
	Task = RecheckOriginalSubscription(OriginalTask, Subscription);
	if (!Task) return;
	if (!Initial.HasRequest())
	{ Task->RejectAndEndTask(Error.IsEmpty() ? TEXT("缺少原首次输入请求") : *Error, Subscription); return; }
	Task->DispatchRequest(Initial, Subscription);
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
	const UGGYGOGameplayAbility* ProjectAbility = Cast<UGGYGOGameplayAbility>(Subscription->OwningAbility.Get());
	if (!ProjectAbility || !ProjectAbility->CaptureCurrentActivation().HasSameActivation(Subscription->Activation))
	{
		Task->RejectAndEndTask(TEXT("原 Task 激活已结束或被同实例后继替换"), Subscription);
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
	const auto Subscription = OriginalInputSubscription;
	UGGYGOAbilityTask_WaitComboInput* Task = RecheckOriginalSubscription(OriginalTask, Subscription);
	if (!Task || Subscription->bRemote) return;
	if (Task->LocalRequestId >= 65535)
	{ Task->RejectAndEndTask(TEXT("原输入请求序号耗尽"), Subscription); return; }
	UGGYGOAbilitySystemComponent* ASC = Cast<UGGYGOAbilitySystemComponent>(Subscription->ASC.Get());
	const int32 OriginalSourceStep = Task->SourceStep;
	const int32 RequestId = Task->LocalRequestId + 1;
	FString Error;
	const auto Request = ASC ? ASC->CaptureStepInputRequest(Task,
		Cast<UGGYGOGameplayAbility>(Subscription->OwningAbility.Get()),
		EGGYGOAbilityStepInputKind::Continue, OriginalSourceStep, RequestId, Error) : FGGYGOAbilityStepInputRequest{};
	Task = RecheckOriginalSubscription(OriginalTask, Subscription);
	if (!Task) return;
	// 只消费本次 local edge；网络等真入场样本完成后走 TargetData。
	Subscription->ASC->ConsumeGenericReplicatedEvent(EAbilityGenericReplicatedEvent::InputPressed,
		Subscription->Spec, Subscription->PredictionKey);
	Task = RecheckOriginalSubscription(OriginalTask, Subscription);
	if (!Task) return;
	if (!Request.HasRequest())
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("WaitComboInput [%s] 拒绝非本次真实 Press 来源：%s。"),
			*Task->GetPathName(), Error.IsEmpty() ? TEXT("原 Press edge 不可用") : *Error);
		return;
	}
	Task->LocalRequestId = RequestId;
	Task->DispatchRequest(Request, Subscription);
}

void UGGYGOAbilityTask_WaitComboInput::HandleTargetData(
	const FGameplayAbilityTargetDataHandle& Data, FGameplayTag ApplicationTag)
{
	const TWeakObjectPtr<UGGYGOAbilityTask_WaitComboInput> OriginalTask(this);
	const auto Subscription = OriginalInputSubscription;
	UGGYGOAbilityTask_WaitComboInput* Task = RecheckOriginalSubscription(OriginalTask, Subscription);
	if (!Task || !Subscription->bRemote) return;
	FGGYGOAbilityStepInputRequest Request;
	if (Data.Num() == 1 && !ApplicationTag.IsValid() && Data.Get(0)
		&& Data.Get(0)->GetScriptStruct() == FGGYGOAbilityStepInputTargetData::StaticStruct())
	{
		// 复制 owned 封装后再 Consume；不跨缓存清理借用 Data。
		Request = static_cast<const FGGYGOAbilityStepInputTargetData*>(Data.Get(0))->OwnedRequest;
	}
	Task = RecheckOriginalSubscription(OriginalTask, Subscription);
	if (!Task) return;
	UGGYGOAbilitySystemComponent* ASC = Cast<UGGYGOAbilitySystemComponent>(Subscription->ASC.Get());
	if (ASC)
	{
		FGameplayAbilityTargetDataHandle Cached;
		ASC->GetAbilityTargetData(Subscription->Spec, Subscription->OwningAbility->GetCurrentActivationInfo(), Cached);
		bool bSameCache = Cached.Num() == Data.Num();
		for (int32 Index = 0; bSameCache && Index < Cached.Num(); ++Index)
			bSameCache = Cached.Get(Index) == Data.Get(Index);
		// 同 Spec/key 可能已有后继包；只消费仍是本次原对象集合的桶。
		if (bSameCache) ASC->ConsumeClientReplicatedTargetData(Subscription->Spec, Subscription->PredictionKey);
	}
	Task = RecheckOriginalSubscription(OriginalTask, Subscription);
	if (!Task) return;
	FString Error;
	if (!Request.HasRequest() || !Request.IsRemote() || !Request.HasSubmittedEntryFact() || !ASC
		|| !ASC->CheckStepInputRequestForTask(Request, Subscription->OwningAbility.Get(), Subscription->Activation, Error))
	{
		// 旧缓存可能与后继复用 Spec/key；拒它自身，不结束后继 Task 或消费其初段状态。
		UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("WaitComboInput [%s] 丢弃非本原激活的 owned 数据：%s。"),
			*Task->GetPathName(), Error.IsEmpty() ? TEXT("缺少原 ASC 受验封装") : *Error);
		return;
	}
	if (Request.GetKind() == EGGYGOAbilityStepInputKind::Initial)
	{
		if (Task->bInitialReceived) return;
		Task->bInitialReceived = true;
		// 首次数据已验收，后续续击是可选输入，不再保留原生的远端必需数据等待。
		Task->ClearWaitingOnRemotePlayerData();
	}
	else
	{
		if (!Task->bInitialReceived || Request.GetRequestId() <= Task->LastRemoteRequestId) return;
		Task->LastRemoteRequestId = Request.GetRequestId();
	}
	Task->DispatchRequest(Request, Subscription);
}

void UGGYGOAbilityTask_WaitComboInput::HandleInitialUnavailable()
{
	const TWeakObjectPtr<UGGYGOAbilityTask_WaitComboInput> OriginalTask(this);
	const auto Subscription = OriginalInputSubscription;
	// 失败归还不要求 ResourceWork Capture；来源可能正因失效而需要结束原 pending。
	const auto RecheckCleanupTask = [&]() -> UGGYGOAbilityTask_WaitComboInput*
	{
		auto* Candidate = OriginalTask.Get();
		if (!Candidate || Candidate->bEndingTask || Candidate->GetState() == EGameplayTaskState::Finished
			|| !Subscription.IsValid() || Candidate->OriginalInputSubscription != Subscription
			|| !Subscription->ASC.IsValid() || !Subscription->OwningAbility.IsValid()
			|| !Subscription->ASC.HasSameIndexAndSerialNumber(Candidate->AbilitySystemComponent)
			|| !Subscription->OwningAbility.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGameplayAbility>(Candidate->Ability))) return nullptr;
		return Candidate;
	};
	UGGYGOAbilityTask_WaitComboInput* Task = RecheckCleanupTask();
	if (!Task || !Subscription->bRemote || Task->bInitialReceived) return;
	const FGGYGOAbilityInitialInputRoute Route = Task->InitialInputRoute;
	UGGYGOAbilitySystemComponent* ASC = Cast<UGGYGOAbilitySystemComponent>(Subscription->ASC.Get());
	if (!ASC || !ASC->ConsumeInitialInputUnavailable(Route, Subscription->Activation)) return;
	Task = RecheckCleanupTask();
	if (!Task) return;
	const auto Snapshot = Task->NativeCallbackRegistration;
	if (!Snapshot.IsValid() || !Snapshot->InitialUnavailable.IsBound())
	{ Task->RejectAndEndTask(TEXT("原 Initial 不可提供事实缺少 GA 消费者"), Subscription); return; }
	Snapshot->InitialUnavailable.Execute();
}

void UGGYGOAbilityTask_WaitComboInput::DispatchRequest(const FGGYGOAbilityStepInputRequest& Request,
	const TSharedPtr<FOriginalInputSubscription>& Subscription)
{
	const TWeakObjectPtr<UGGYGOAbilityTask_WaitComboInput> OriginalTask(this);
	UGGYGOAbilityTask_WaitComboInput* Task = RecheckOriginalSubscription(OriginalTask, Subscription);
	if (!Task) return;
	TSharedPtr<FNativeCallbackRegistration> Snapshot = Task->NativeCallbackRegistration;
	if (!Snapshot.IsValid())
	{ Task->RejectAndEndTask(TEXT("原 owned 请求缺少 native 消费者"), Subscription); return; }
	Snapshot->Callback.ExecuteIfBound(Request);
	Snapshot.Reset();
	Task = RecheckOriginalSubscription(OriginalTask, Subscription);
	if (Task && Request.GetKind() == EGGYGOAbilityStepInputKind::Continue)
		Task->OnPress.Broadcast(Request.GetSourceStep(), Request.GetRequestId());
}

bool UGGYGOAbilityTask_WaitComboInput::SubmitEntryFact(const FGGYGOAbilityStepInputRequest& Request,
	const FGGYGOMovementInputStepFact& Fact, FString& OutError)
{
	OutError.Reset();
	const TWeakObjectPtr<UGGYGOAbilityTask_WaitComboInput> OriginalTask(this);
	const auto Subscription = OriginalInputSubscription;
	UGGYGOAbilityTask_WaitComboInput* Task = RecheckOriginalSubscription(OriginalTask, Subscription);
	if (!Task || Subscription->bRemote) { OutError = TEXT("原本地 Task 不可提交"); return false; }
	const auto OriginalFact = Fact;
	UGGYGOAbilitySystemComponent* ASC = Cast<UGGYGOAbilitySystemComponent>(Subscription->ASC.Get());
	if (!ASC) { OutError = TEXT("原 ASC 不支持 owned 输入"); return false; }
	const bool bInitial = Request.GetKind() == EGGYGOAbilityStepInputKind::Initial;
	if ((bInitial && Task->InitialInputRoute.HasSubmittedInitial()) || (!bInitial && !Task->InitialInputRoute.HasSubmittedInitial()))
	{ OutError = TEXT("步骤请求不符合原 Initial 一次提交后才可 Continue 的运输顺序。"); return false; }
	if (!Subscription->bPredicting && (!ASC->GetOwnerActor() || !ASC->GetOwnerActor()->HasAuthority()))
	{ OutError = TEXT("步骤事实需要本地 Authority 或原 LocalPredicted 发送通路。"); return false; }
	FScopedPredictionWindow Prediction(ASC, Subscription->bPredicting);
	Task = RecheckOriginalSubscription(OriginalTask, Subscription);
	if (!Task) { OutError = TEXT("预测窗口返回后原 Task 已失效"); return false; }
	if (!ASC->CheckStepInputRequestForTask(Request, Subscription->OwningAbility.Get(), Subscription->Activation, OutError)) return false;
	if (!ASC->CommitStepInputFact(Task, Request, OriginalFact, Task->InitialInputRoute, OutError)) return false;
	if (Subscription->bPredicting)
	{
		auto* Payload = new FGGYGOAbilityStepInputTargetData;
		Payload->Kind = Request.GetKind(); Payload->SourceStep = Request.GetSourceStep();
		Payload->RequestId = Request.GetRequestId(); Payload->StepFact = OriginalFact;
		const FGameplayAbilityTargetDataHandle Packet(Payload);
		UE_LOG(LogGGYGOAbilitySystem, Display,
			TEXT("[AbilitySystem.OwnedStepInput] Boundary=SendAttempt ASC='%s' World='%s' Spec=%s ActivationKey=%s ScopedKey=%s Kind=%u SourceStep=%d Request=%d Edge=%llu Entry=%llu Frame=%llu"),
			*ASC->GetPathName(), *GetPathNameSafe(ASC->GetWorld()), *Subscription->Spec.ToString(),
			*Subscription->PredictionKey.ToString(), *ASC->ScopedPredictionKey.ToString(), static_cast<uint32>(Payload->Kind),
			Payload->SourceStep, Payload->RequestId, static_cast<unsigned long long>(OriginalFact.OriginalEdgeEvaluationSerial),
			static_cast<unsigned long long>(OriginalFact.StepEvaluationSerial), static_cast<unsigned long long>(GFrameCounter));
		ASC->ServerSetReplicatedTargetData(Subscription->Spec, Subscription->PredictionKey,
			Packet, FGameplayTag{}, ASC->ScopedPredictionKey);
	}
	return true;
}

void UGGYGOAbilityTask_WaitComboInput::OnDestroy(bool AbilityEnded)
{
	if (bEndingTask) { return; }
	bEndingTask = true;
	TSharedPtr<FNativeCallbackRegistration> DetachedNative = MoveTemp(NativeCallbackRegistration);
	TSharedPtr<FOriginalInputSubscription> DetachedSubscription = MoveTemp(OriginalInputSubscription);
	const FGGYGOAbilityInitialInputRoute EndingRoute = MoveTemp(InitialInputRoute);
	if (DetachedSubscription.IsValid())
	{
		if (UAbilitySystemComponent* ASC = DetachedSubscription->ASC.Get())
		{
			if (DetachedSubscription->bRemote)
			{
				ASC->AbilityTargetDataSetDelegate(DetachedSubscription->Spec, DetachedSubscription->PredictionKey).Remove(DetachedSubscription->Handle);
				ASC->AbilityTargetDataCancelledDelegate(DetachedSubscription->Spec, DetachedSubscription->PredictionKey).Remove(DetachedSubscription->CancellationHandle);
			}
			else ASC->AbilityReplicatedEventDelegate(EAbilityGenericReplicatedEvent::InputPressed,
				DetachedSubscription->Spec, DetachedSubscription->PredictionKey).Remove(DetachedSubscription->Handle);
		}
	}
	EndingRoute.NotifyInitialUnavailable();
	Super::OnDestroy(AbilityEnded);
	DetachedNative.Reset(); // Own cleanup/Super returned; no task access after capture destruction.
}

/**
 * @file GGYGOAbilityInputBuffer.cpp
 * @brief ASC 能力输入缓冲实现：请求身份、按住聚合、边沿、组排队重试与每帧消费。
 */
#include "AbilitySystem/Private/GGYGOAbilityInputBuffer.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/Private/GGYGOAbilityStepInputRouter.h"
#include "System/GGYGOGameplayTags.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/ScopeExit.h"

FGGYGOAbilityInputBuffer::FGGYGOAbilityInputBuffer(UGGYGOAbilitySystemComponent& InASC)
	: ASC(InASC)
{
}

FGGYGOAbilityInputRequestResult FGGYGOAbilityInputBuffer::MakeAbilityInputRequestFailure(
	EGGYGOAbilityInputRequestOutcome Outcome, EGGYGOAbilityInputRequestReason Reason,
	const FGGYGOAbilityInputRequestIdentity& Identity, FGameplayTag Tag, double Deadline) const
{
	static const TCHAR* const Names[] = {
		TEXT("None"), TEXT("InvalidRequest"), TEXT("InvalidTag"), TEXT("InvalidDeadline"),
		TEXT("InvalidPreviousIdentity"), TEXT("InvalidASC"),
		TEXT("WrongASC"), TEXT("WrongRevision"), TEXT("UnknownRequest"), TEXT("RequestEnded"),
		TEXT("RequestMismatch"), TEXT("ActorInfoUnavailable"), TEXT("InputBlocked"),
		TEXT("NoMatchingSpec"), TEXT("Expired"), TEXT("NoQueuedAdmission"), TEXT("NotHeld")
	};
	const int32 Index = static_cast<int32>(Reason);
	const TCHAR* Name = Index >= 0 && Index < UE_ARRAY_COUNT(Names) ? Names[Index] : TEXT("InvalidReason");
	UE_LOG(LogGGYGOAbilitySystem, Warning,
		TEXT("AbilitySystem InputRequest rejected: ASC=%s Source=%s Tag=%s Revision=%llu Request=%llu Deadline=%.6f Outcome=%d Reason=%s"),
		*ASC.GetPathName(), *GetPathNameSafe(Identity.SourceASC.Get()), *Tag.ToString(),
		Identity.InputRevision, Identity.RequestSerial, Deadline, static_cast<int32>(Outcome), Name);
	FGGYGOAbilityInputRequestResult Result;
	Result.Outcome = Outcome;
	Result.Reason = Reason;
	return Result; // Failure never issues an identity or substitutes another request.
}

const FGGYGOAbilityInputBuffer::FAbilityInputRequestRecord*
FGGYGOAbilityInputBuffer::FindAbilityInputRequest(const FGGYGOAbilityInputRequestIdentity& Identity) const
{
	if (!Identity.IsAssigned() || Identity.InputRevision != AbilityInputRevision
		|| !Identity.SourceASC.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGGYGOAbilitySystemComponent>(
			const_cast<UGGYGOAbilitySystemComponent*>(&ASC))))
	{
		return nullptr;
	}
	const FAbilityInputRequestRecord* Record = AbilityInputRequests.Find(Identity.RequestSerial);
	return Record && Record->Request.Identity == Identity ? Record : nullptr;
}

bool FGGYGOAbilityInputBuffer::IsAbilityInputActorInfoUsable() const
{
	const AActor* const Owner = ASC.GetOwnerActor();
	const AActor* const Avatar = ASC.GetAvatarActor();
	return IsValid(&ASC) && !ASC.HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		&& ASC.AbilityActorInfo.IsValid() && ASC.AbilityActorInfo->AbilitySystemComponent.Get() == &ASC
		&& IsValid(Owner) && IsValid(Avatar) && !Owner->IsActorBeingDestroyed() && !Avatar->IsActorBeingDestroyed()
		&& IsValid(ASC.GetWorld());
}

bool FGGYGOAbilityInputBuffer::RequestMatchesSpec(const FAbilityInputRequestRecord& Record,
	FGameplayAbilitySpecHandle Handle) const
{
	if (!Record.SpecHandles.Contains(Handle)) { return false; }
	const FGameplayAbilitySpec* Spec = ASC.FindAbilitySpecFromHandle(Handle);
	return Spec && Spec->Ability && !Spec->PendingRemove && !Spec->RemoveAfterActivation
		&& Spec->GetDynamicSpecSourceTags().HasTagExact(Record.Request.InputTag);
}

bool FGGYGOAbilityInputBuffer::IsBeforeDeadline(const FAbilityInputRequestRecord& Record) const
{
	const UWorld* const World = ASC.GetWorld();
	return World && FMath::IsFinite(Record.Request.OriginalDeadline)
		&& static_cast<double>(World->GetTimeSeconds()) < Record.Request.OriginalDeadline;
}

bool FGGYGOAbilityInputBuffer::IsAbilityInputEdgeCurrent(const FAbilityInputEdge& Edge) const
{
	if (Edge.InputRevision != AbilityInputRevision) { return false; }
	if (!Edge.bPressed)
	{
		return true; // A recorded real release is history, not a new request or a retry permit.
	}
	for (const FGGYGOAbilityInputRequestIdentity& Identity : Edge.Sources)
	{
		const FAbilityInputRequestRecord* Record = FindAbilityInputRequest(Identity);
		if (Record && RequestMatchesSpec(*Record, Edge.Handle)) { return true; }
	}
	return false;
}

void FGGYGOAbilityInputBuffer::RebuildAbilityInputHeldHandles()
{
	InputHeldSpecHandles.Reset();
	TArray<uint64> Serials;
	AbilityInputRequests.GetKeys(Serials);
	Serials.Sort();
	for (const uint64 Serial : Serials)
	{
		const FAbilityInputRequestRecord& Record = AbilityInputRequests.FindChecked(Serial);
		if (Record.bHeld)
		{
			for (const FGameplayAbilitySpecHandle Handle : Record.SpecHandles)
			{
				if (RequestMatchesSpec(Record, Handle)) { InputHeldSpecHandles.AddUnique(Handle); }
			}
		}
	}
}

void FGGYGOAbilityInputBuffer::PruneAbilityInputRequests()
{
	TArray<FGGYGOAbilityInputRequestIdentity> Retired;
	for (TPair<uint64, FAbilityInputRequestRecord>& Entry : AbilityInputRequests)
	{
		FAbilityInputRequestRecord& Record = Entry.Value;
		const bool bRetryExpired = !IsBeforeDeadline(Record);
		if (bRetryExpired)
		{
			Record.RetryableSpecHandles.Reset();
			Record.QueuedWaitOrder = 0;
			PendingAbilityInputRetries.RemoveAll([&Record](const FGGYGOAbilityInputRetryRequest& Pending)
				{ return Pending.Identity == Record.Request.Identity; });
		}
		// Held survives its retry deadline. Released storage also survives pending real edges/in-flight snapshots.
		if (!Record.bHeld && !bProcessingAbilityInput
			&& bRetryExpired
			&& !PendingAbilityInputEdges.ContainsByPredicate([&Record](const FAbilityInputEdge& Edge)
				{ return Edge.Sources.Contains(Record.Request.Identity); }))
		{
			Retired.Add(Record.Request.Identity);
		}
	}
	for (const FGGYGOAbilityInputRequestIdentity& Identity : Retired)
	{
		EndAbilityInputRequest(Identity, EGGYGOAbilityInputRequestEndKind::Invalidated);
	}
	RebuildAbilityInputHeldHandles();
}

void FGGYGOAbilityInputBuffer::ConsumeSuccessfulAbilityInputRequests(
	const TArray<FGGYGOAbilityInputRetryRequest>& Requests)
{
	for (const FGGYGOAbilityInputRetryRequest& Request : Requests)
	{
		FAbilityInputRequestRecord* Record = AbilityInputRequests.Find(Request.Identity.RequestSerial);
		if (Record && Record->Request.Identity == Request.Identity
			&& Record->Request.InputTag == Request.InputTag
			&& Record->Request.OriginalDeadline == Request.OriginalDeadline)
		{
			Record->RetryableSpecHandles.Reset();
			PendingAbilityInputRetries.RemoveAll([&Request](const FGGYGOAbilityInputRetryRequest& Pending)
				{ return Pending.Identity == Request.Identity; });
		}
	}
}

FGGYGOAbilityInputRequestResult FGGYGOAbilityInputBuffer::ReceiveAbilityInputRequest(
	const FGameplayTag& InputTag, const FGGYGOAbilityInputRequestIdentity& PreviousIdentity, double OriginalDeadline)
{
	check(IsInGameThread());
	// 普通调用不采新方向；既有 ID 复核只搬它自己首次记录的来源。
	const FAbilityInputRequestRecord* Existing = FindAbilityInputRequest(PreviousIdentity);
	const FGGYGOMovementInputEvaluationToken Original = Existing
		? Existing->Request.OriginalEvaluationToken : FGGYGOMovementInputEvaluationToken{};
	return ReceiveAbilityInputRequest(InputTag, PreviousIdentity, OriginalDeadline, Original,
		Existing ? Existing->Source : FGGYGOAbilityInputSourceKey{});
}

FGGYGOAbilityInputRequestResult FGGYGOAbilityInputBuffer::ReceiveAbilityInputRequest(
	const FGameplayTag& InputTag, const FGGYGOAbilityInputRequestIdentity& PreviousIdentity, double OriginalDeadline,
	const FGGYGOMovementInputEvaluationToken& OriginalEvaluationToken, const FGGYGOAbilityInputSourceKey& Source)
{
	check(IsInGameThread());
	using EOutcome = EGGYGOAbilityInputRequestOutcome;
	using EReason = EGGYGOAbilityInputRequestReason;
	const auto Fail = [&](EOutcome Outcome, EReason Reason)
		{ return MakeAbilityInputRequestFailure(Outcome, Reason, PreviousIdentity, InputTag, OriginalDeadline); };
	if (!IsValid(&ASC) || ASC.HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) { return Fail(EOutcome::Rejected, EReason::InvalidASC); }
	if (!InputTag.IsValid()) { return Fail(EOutcome::Rejected, EReason::InvalidTag); }
	if (!FMath::IsFinite(OriginalDeadline) || OriginalDeadline < 0.0) { return Fail(EOutcome::Rejected, EReason::InvalidDeadline); }
	const bool bEmptyPrevious = PreviousIdentity.RequestSerial == 0 && PreviousIdentity.InputRevision == 0
		&& PreviousIdentity.SourceASC.IsExplicitlyNull();
	if (!bEmptyPrevious && !PreviousIdentity.IsAssigned()) { return Fail(EOutcome::Rejected, EReason::InvalidPreviousIdentity); }
	PruneAbilityInputRequests();
	if (!bEmptyPrevious)
	{
		if (!PreviousIdentity.SourceASC.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGGYGOAbilitySystemComponent>(&ASC)))
			{ return Fail(EOutcome::Stale, EReason::WrongASC); }
		if (PreviousIdentity.InputRevision != AbilityInputRevision) { return Fail(EOutcome::Stale, EReason::WrongRevision); }
		const FAbilityInputRequestRecord* Existing = FindAbilityInputRequest(PreviousIdentity);
		if (!Existing) { return Fail(EOutcome::Stale, EReason::UnknownRequest); }
		if (Existing->Request.InputTag != InputTag || Existing->Request.OriginalDeadline != OriginalDeadline
			|| !(Existing->Request.OriginalEvaluationToken == OriginalEvaluationToken) || !(Existing->Source == Source))
			{ return Fail(EOutcome::Rejected, EReason::RequestMismatch); }
		if (!Existing->bHeld) { return Fail(EOutcome::Stale, EReason::RequestEnded); }
		if (ASC.HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked)) { return Fail(EOutcome::Rejected, EReason::InputBlocked); }
		return FGGYGOAbilityInputRequestResult{EOutcome::AlreadyApplied, EReason::None, Existing->Request.Identity};
	}
	if (!IsAbilityInputActorInfoUsable()) { return Fail(EOutcome::Rejected, EReason::ActorInfoUnavailable); }
	if (ASC.HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked)) { return Fail(EOutcome::Rejected, EReason::InputBlocked); }
	if (static_cast<double>(ASC.GetWorld()->GetTimeSeconds()) > OriginalDeadline) { return Fail(EOutcome::Rejected, EReason::Expired); }
	FAbilityInputRequestRecord Record;
	Record.Request.InputTag = InputTag;
	Record.Request.OriginalDeadline = OriginalDeadline;
	Record.Request.OriginalEvaluationToken = OriginalEvaluationToken;
	Record.Source = Source;
	for (const FGameplayAbilitySpec& Spec : ASC.ActivatableAbilities.Items)
	{
		if (Spec.Ability && !Spec.PendingRemove && !Spec.RemoveAfterActivation
			&& Spec.GetDynamicSpecSourceTags().HasTagExact(InputTag))
		{
			Record.SpecHandles.Add(Spec.Handle);
		}
	}
	if (Record.SpecHandles.IsEmpty()) { return Fail(EOutcome::Rejected, EReason::NoMatchingSpec); }
	Record.Request.Identity = FGGYGOAbilityInputRequestIdentity{&ASC, AbilityInputRevision, ++LastAbilityInputRequestSerial};
	const FGGYGOAbilityInputRequestIdentity Identity = Record.Request.Identity;
	const TArray<FGameplayAbilitySpecHandle> PreviousHeld = InputHeldSpecHandles;
	const TArray<FGameplayAbilitySpecHandle> ReceivedSpecs = Record.SpecHandles;
	AbilityInputRequests.Add(Identity.RequestSerial, MoveTemp(Record));
	for (const FGameplayAbilitySpecHandle Handle : ReceivedSpecs)
	{
		if (!PreviousHeld.Contains(Handle))
		{
			FAbilityInputEdge Edge;
			Edge.Handle = Handle;
			Edge.InputRevision = AbilityInputRevision;
			Edge.bPressed = true;
			Edge.OriginalEvaluationToken = OriginalEvaluationToken;
			Edge.Sources.Add(Identity);
			PendingAbilityInputEdges.Add(MoveTemp(Edge));
		}
		else
		{
			// A second source contributes to the same unconsumed aggregate press; never emits another press.
			for (int32 Index = PendingAbilityInputEdges.Num() - 1; Index >= 0; --Index)
			{
				FAbilityInputEdge& Edge = PendingAbilityInputEdges[Index];
				if (Edge.Handle == Handle)
				{
					if (Edge.bPressed) { Edge.Sources.AddUnique(Identity); }
					break;
				}
			}
		}
	}
	RebuildAbilityInputHeldHandles();
	return FGGYGOAbilityInputRequestResult{EOutcome::Accepted, EReason::None, Identity};
}

FGGYGOAbilityInputRequestResult FGGYGOAbilityInputBuffer::EndAbilityInputRequest(
	const FGGYGOAbilityInputRequestIdentity& Identity, EGGYGOAbilityInputRequestEndKind EndKind)
{
	check(IsInGameThread());
	using EOutcome = EGGYGOAbilityInputRequestOutcome;
	using EReason = EGGYGOAbilityInputRequestReason;
	const auto Fail = [&](EOutcome Outcome, EReason Reason)
		{ return MakeAbilityInputRequestFailure(Outcome, Reason, Identity, FGameplayTag{}, UGGYGOAbilitySystemComponent::NoAbilityInputRetryDeadline); };
	if (!IsValid(&ASC) || ASC.HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) { return Fail(EOutcome::Rejected, EReason::InvalidASC); }
	if (!Identity.IsAssigned()) { return Fail(EOutcome::Rejected, EReason::InvalidRequest); }
	if (!Identity.SourceASC.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGGYGOAbilitySystemComponent>(&ASC)))
		{ return Fail(EOutcome::Stale, EReason::WrongASC); }
	if (Identity.InputRevision != AbilityInputRevision) { return Fail(EOutcome::Stale, EReason::WrongRevision); }
	FAbilityInputRequestRecord* Record = AbilityInputRequests.Find(Identity.RequestSerial);
	if (!Record || Record->Request.Identity != Identity) { return Fail(EOutcome::Stale, EReason::UnknownRequest); }
	const TArray<FGameplayAbilitySpecHandle> Affected = Record->SpecHandles;
	if (EndKind == EGGYGOAbilityInputRequestEndKind::Released)
	{
		if (!Record->bHeld) { return FGGYGOAbilityInputRequestResult{EOutcome::AlreadyApplied, EReason::None, Identity}; }
		Record->bHeld = false; // Deadline does not gate a true release.
		RebuildAbilityInputHeldHandles();
		for (const FGameplayAbilitySpecHandle Handle : Affected)
		{
			if (!InputHeldSpecHandles.Contains(Handle))
			{
				FAbilityInputEdge Edge;
				Edge.Handle = Handle;
				Edge.InputRevision = AbilityInputRevision;
				Edge.Sources.Add(Identity);
				PendingAbilityInputEdges.Add(MoveTemp(Edge));
			}
		}
	}
	else
	{
		// Original-ID retirement needs neither Ready, current actor context nor a live deadline.
		AbilityInputRequests.Remove(Identity.RequestSerial);
		PendingAbilityInputRetries.RemoveAll([&Identity](const FGGYGOAbilityInputRetryRequest& Request)
			{ return Request.Identity == Identity; });
		for (FAbilityInputEdge& Edge : PendingAbilityInputEdges)
		{
			if (Edge.bPressed) { Edge.Sources.Remove(Identity); }
		}
		PendingAbilityInputEdges.RemoveAll([](const FAbilityInputEdge& Edge)
			{ return Edge.bPressed && Edge.Sources.IsEmpty(); });
		RebuildAbilityInputHeldHandles();
		for (const FGameplayAbilitySpecHandle Handle : Affected)
		{
			if (!InputHeldSpecHandles.Contains(Handle))
			{
				if (FGameplayAbilitySpec* Spec = ASC.FindAbilitySpecFromHandle(Handle)) { Spec->InputPressed = false; }
			}
		}
	}
	return FGGYGOAbilityInputRequestResult{EOutcome::Accepted, EReason::None, Identity};
}

void FGGYGOAbilityInputBuffer::EndAbilityInputRequestsFromSource(const FGGYGOAbilityInputSourceKey& Source)
{
	check(IsInGameThread());
	if (!Source.IsSet()) { return; }
	TArray<FGGYGOAbilityInputRequestIdentity> Owned;
	for (const TPair<uint64, FAbilityInputRequestRecord>& Entry : AbilityInputRequests)
	{
		if (Entry.Value.Source == Source) { Owned.Add(Entry.Value.Request.Identity); }
	}
	for (const FGGYGOAbilityInputRequestIdentity& Identity : Owned)
	{
		EndAbilityInputRequest(Identity, EGGYGOAbilityInputRequestEndKind::Invalidated);
	}
}

FGGYGOAbilityInputRequestResult FGGYGOAbilityInputBuffer::QueueAbilityInputRetry(
	const FGGYGOAbilityInputRetryRequest& Request)
{
	check(IsInGameThread());
	using EOutcome = EGGYGOAbilityInputRequestOutcome;
	using EReason = EGGYGOAbilityInputRequestReason;
	const auto Fail = [](EOutcome Outcome, EReason Reason)
	{
		FGGYGOAbilityInputRequestResult Result;
		Result.Outcome = Outcome;
		Result.Reason = Reason;
		return Result;
	};
	if (!IsValid(&ASC) || ASC.HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) { return Fail(EOutcome::Rejected, EReason::InvalidASC); }
	if (!Request.InputTag.IsValid()) { return Fail(EOutcome::Rejected, EReason::InvalidTag); }
	if (!Request.Identity.IsAssigned()) { return Fail(EOutcome::Rejected, EReason::InvalidRequest); }
	if (!FMath::IsFinite(Request.OriginalDeadline) || Request.OriginalDeadline < 0.0)
		{ return Fail(EOutcome::Rejected, EReason::InvalidDeadline); }
	if (!Request.Identity.SourceASC.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGGYGOAbilitySystemComponent>(&ASC)))
		{ return Fail(EOutcome::Stale, EReason::WrongASC); }
	if (Request.Identity.InputRevision != AbilityInputRevision) { return Fail(EOutcome::Stale, EReason::WrongRevision); }
	const FAbilityInputRequestRecord* Record = FindAbilityInputRequest(Request.Identity);
	if (!Record) { return Fail(EOutcome::Stale, EReason::UnknownRequest); }
	if (Record->Request.InputTag != Request.InputTag || Record->Request.OriginalDeadline != Request.OriginalDeadline)
		{ return Fail(EOutcome::Rejected, EReason::RequestMismatch); }
	if (ASC.HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked)) { return Fail(EOutcome::Rejected, EReason::InputBlocked); }
	if (!IsBeforeDeadline(*Record)) { return Fail(EOutcome::Stale, EReason::Expired); }
	bool bEligible = false;
	bool bNeedsHeld = false;
	for (const FGameplayAbilitySpecHandle Handle : Record->RetryableSpecHandles)
	{
		const FGameplayAbilitySpec* Spec = ASC.FindAbilitySpecFromHandle(Handle);
		const UGGYGOGameplayAbility* CDO = Spec ? Cast<UGGYGOGameplayAbility>(Spec->Ability) : nullptr;
		if (!CDO || Spec->IsActive()) { continue; }
		const EGGYGOAbilityActivationPolicy Policy = CDO->GetActivationPolicy();
		const bool bWhileHeld = Policy == EGGYGOAbilityActivationPolicy::WhileInputActive;
		bNeedsHeld |= bWhileHeld && !Record->bHeld;
		bEligible |= (Policy == EGGYGOAbilityActivationPolicy::OnInputTriggered || bWhileHeld)
			&& RequestMatchesSpec(*Record, Handle) && (!bWhileHeld || Record->bHeld);
	}
	if (!bEligible) { return Fail(EOutcome::Rejected, bNeedsHeld ? EReason::NotHeld : EReason::NoQueuedAdmission); }
	const bool bQueued = PendingAbilityInputRetries.ContainsByPredicate([&Request](const FGGYGOAbilityInputRetryRequest& Pending)
		{ return Pending.Identity == Request.Identity; });
	if (!bQueued) { PendingAbilityInputRetries.Add(Request); }
	return FGGYGOAbilityInputRequestResult{bQueued ? EOutcome::AlreadyApplied : EOutcome::Accepted, EReason::None, Request.Identity};
}

void FGGYGOAbilityInputBuffer::RecordAbilityInputQueuedWait(FAbilityInputRequestRecord& Record)
{
	check(IsInGameThread());
	if (Record.QueuedWaitOrder != 0) { return; }
	// 只表达首次失败通知的顺序，不是身份；64 位单调计数不判耗尽。
	Record.QueuedWaitOrder = ++LastAbilityInputWaitOrder;
}

void FGGYGOAbilityInputBuffer::WakeQueuedAbilityInputRequests()
{
	check(IsInGameThread());
	TArray<TPair<uint64, FGGYGOAbilityInputRetryRequest>> WaitingRequests;
	for (TPair<uint64, FAbilityInputRequestRecord>& Entry : AbilityInputRequests)
	{
		FAbilityInputRequestRecord& Record = Entry.Value;
		if (Record.QueuedWaitOrder != 0)
		{
			WaitingRequests.Emplace(Record.QueuedWaitOrder, Record.Request);
			Record.QueuedWaitOrder = 0; // Take the whole wait set before any queue/observer callback.
		}
	}
	LastAbilityInputWaitOrder = 0;
	WaitingRequests.Sort([](const auto& A, const auto& B) { return A.Key < B.Key; });
	for (const TPair<uint64, FGGYGOAbilityInputRetryRequest>& Waiting : WaitingRequests)
	{
		// Active, blocked, expired and otherwise rejected candidates are also consumed.
		// Only a new actual Queued failure can put the original request at the tail again.
		QueueAbilityInputRetry(Waiting.Value);
	}
}

void FGGYGOAbilityInputBuffer::ProcessAbilityInput(float DeltaTime, bool bGamePaused)
{
	check(IsInGameThread());
	if (bProcessingAbilityInput) { return; }
	// 入口核对一次 ActorInfo；ActorInfo 变化本身会清空输入并递增 Revision，外调之后只核 Revision。
	if (ASC.HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked) || !IsAbilityInputActorInfoUsable())
	{
		ClearAbilityInput();
		return;
	}
	if (bAwaitingSwitchReturn && !ASC.HasMatchingGameplayTag(GGYGOGameplayTags::State_SwitchReturnPending))
	{
		// 与组释放同一路径：许可排入本次消费，截止已过的请求在入队时被拒。
		bAwaitingSwitchReturn = false;
		WakeQueuedAbilityInputRequests();
	}
	PruneAbilityInputRequests();
	TGuardValue<bool> ProcessingGuard(bProcessingAbilityInput, true);
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(&ASC);
	const uint64 ProcessingRevision = AbilityInputRevision;
	const uint64 LastSnapshotRequestSerial = LastAbilityInputRequestSerial;
	const TArray<FGameplayAbilitySpecHandle> HeldHandles = InputHeldSpecHandles;
	// 只供失败来源匹配原 Owner，不参与请求有效性。
	const TWeakObjectPtr<AActor> OriginalOwner(ASC.GetOwnerActor());
	const TArray<FAbilityInputEdge> Edges = MoveTemp(PendingAbilityInputEdges);
	const TArray<FGGYGOAbilityInputRetryRequest> Retries = MoveTemp(PendingAbilityInputRetries);
	PendingAbilityInputEdges.Reset();
	PendingAbilityInputRetries.Reset();

	struct FRetryWork
	{
		FGGYGOAbilityInputRetryRequest Request;
		TArray<FGameplayAbilitySpecHandle> Handles;
	};
	TArray<FRetryWork> RetryWork;
	TArray<TArray<FGGYGOAbilityInputRetryRequest>> InitialSourcesByEdge;
	InitialSourcesByEdge.SetNum(Edges.Num());
	TMap<FGameplayAbilitySpecHandle, TArray<FGGYGOAbilityInputRetryRequest>> HeldSources;
	// Freeze each press edge's own sources before the first native event or Try callback.
	for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
	{
		const FAbilityInputEdge& Edge = Edges[EdgeIndex];
		if (!Edge.bPressed) { continue; }
		for (const FGGYGOAbilityInputRequestIdentity& Identity : Edge.Sources)
		{
			if (const FAbilityInputRequestRecord* Record = FindAbilityInputRequest(Identity))
			{
				TArray<FGGYGOAbilityInputRetryRequest>& Sources = InitialSourcesByEdge[EdgeIndex];
				if (!Sources.ContainsByPredicate([&Identity](const FGGYGOAbilityInputRetryRequest& Request)
					{ return Request.Identity == Identity; })) { Sources.Add(Record->Request); }
			}
		}
	}
	for (const TPair<uint64, FAbilityInputRequestRecord>& Entry : AbilityInputRequests)
	{
		const FAbilityInputRequestRecord& Record = Entry.Value;
		if (Record.bHeld)
		{
			for (const FGameplayAbilitySpecHandle Handle : Record.SpecHandles)
			{
				if (RequestMatchesSpec(Record, Handle)) { HeldSources.FindOrAdd(Handle).Add(Record.Request); }
			}
		}
	}
	for (const FGGYGOAbilityInputRetryRequest& Request : Retries)
	{
		if (const FAbilityInputRequestRecord* Record = FindAbilityInputRequest(Request.Identity))
		{
			RetryWork.Add(FRetryWork{Request, Record->RetryableSpecHandles});
		}
	}
	const auto CanContinue = [&, OriginalASC]()
	{
		if (!OriginalASC.IsValid()) { return false; }
		if (ASC.HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked))
		{
			ClearAbilityInput();
			return false;
		}
		return ProcessingRevision == AbilityInputRevision;
	};
	const auto HasSupersedingInput = [&](FGameplayAbilitySpecHandle Handle)
	{
		// Native callbacks add facts to next consumption; an old batch must not overwrite them.
		if (PendingAbilityInputEdges.ContainsByPredicate([Handle](const FAbilityInputEdge& Edge)
			{ return Edge.Handle == Handle; })) { return true; }
		for (const TPair<uint64, FAbilityInputRequestRecord>& Entry : AbilityInputRequests)
		{
			if (Entry.Key > LastSnapshotRequestSerial && Entry.Value.bHeld
				&& Entry.Value.SpecHandles.Contains(Handle)) { return true; }
		}
		return false;
	};
	TArray<FGameplayAbilitySpecHandle> Attempted;
	TArray<FGameplayAbilitySpecHandle> DeferredHandles;
	TArray<FAbilityInputEdge> DeferredEdges;
	ON_SCOPE_EXIT
	{
		if (!DeferredEdges.IsEmpty() && OriginalASC.IsValid() && ProcessingRevision == AbilityInputRevision)
		{
			// Preserve earlier true edges ahead of callback-created next-frame facts.
			DeferredEdges.Append(MoveTemp(PendingAbilityInputEdges));
			PendingAbilityInputEdges = MoveTemp(DeferredEdges);
		}
	};
	const auto TryOnce = [&](FGameplayAbilitySpecHandle Handle, EAbilityActivationInputOrigin Kind,
		const TArray<FGGYGOAbilityInputRetryRequest>& Sources,
		const FGGYGOMovementInputEvaluationToken& OriginalPressToken = FGGYGOMovementInputEvaluationToken{})
	{
		if (!CanContinue() || Attempted.Contains(Handle) || HasSupersedingInput(Handle)) { return; }
		const FGameplayAbilitySpec* Spec = ASC.FindAbilitySpecFromHandle(Handle);
		const UGGYGOGameplayAbility* CDO = Spec ? Cast<UGGYGOGameplayAbility>(Spec->Ability) : nullptr;
		if (!CDO || Spec->IsActive() || Spec->PendingRemove || Spec->RemoveAfterActivation) { return; }
		const EGGYGOAbilityActivationPolicy Policy = CDO->GetActivationPolicy();
		const bool bWhileHeld = Policy == EGGYGOAbilityActivationPolicy::WhileInputActive;
		if (Policy != EGGYGOAbilityActivationPolicy::OnInputTriggered && !bWhileHeld) { return; }
		const bool bRetry = Kind == EAbilityActivationInputOrigin::FiniteRetry;
		TArray<FGGYGOAbilityInputRetryRequest> CurrentSources;
		for (const FGGYGOAbilityInputRetryRequest& Request : Sources)
		{
			const FAbilityInputRequestRecord* Record = FindAbilityInputRequest(Request.Identity);
			if (Record && RequestMatchesSpec(*Record, Handle) && (!bWhileHeld || Record->bHeld)
				&& (!bRetry || (Record->RetryableSpecHandles.Contains(Handle) && IsBeforeDeadline(*Record))))
			{
				CurrentSources.Add(Request);
			}
		}
		if (CurrentSources.IsEmpty()) { return; }
		FAbilityActivationFailureOrigin Origin;
		Origin.Kind = Kind;
		Origin.Handle = Handle;
		Origin.Requests = CurrentSources;
		Origin.OwnerActor = OriginalOwner;
		Attempted.Add(Handle); // Before external calls; no second Try for the same Spec in this batch.
		// Spend the old Queued permission before native callbacks. Only a new proven failure can renew it.
		for (const FGGYGOAbilityInputRetryRequest& Request : CurrentSources)
		{
			FAbilityInputRequestRecord* Record = AbilityInputRequests.Find(Request.Identity.RequestSerial);
			if (Record && Record->Request.Identity == Request.Identity) { Record->RetryableSpecHandles.Remove(Handle); }
		}
		bool bActivated = false;
		{
			FGGYGOMovementInputEvaluationToken Token = OriginalPressToken;
			if (!Token.IsSet() && Kind != EAbilityActivationInputOrigin::InitialPress)
			{
				// 重试只搬原来源；不同评价的多个来源不能猜成某一个真按下。
				bool bFirstSource = true;
				for (const auto& Source : CurrentSources)
				{
					if (bFirstSource) { Token = Source.OriginalEvaluationToken; bFirstSource = false; }
					else if (!(Token == Source.OriginalEvaluationToken)) { Token = {}; break; }
				}
			}
			const FGGYGOAbilityStepInputRouter::FDispatch Dispatch{Handle, Token, true, ProcessingRevision};
			const FGGYGOAbilityStepInputRouter::FScopedDispatch DispatchScope(*ASC.StepInputRouter, Dispatch);
			FScopedAbilityInputActivation InputScope(*this, Origin);
			bActivated = ASC.TryActivateAbility(Handle);
		}
		if (!CanContinue()) { return; }
		if (bActivated) { ConsumeSuccessfulAbilityInputRequests(CurrentSources); }
	};

	for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
	{
		const FAbilityInputEdge& Edge = Edges[EdgeIndex];
		if (!CanContinue()) { return; }
		if (DeferredHandles.Contains(Edge.Handle)) { DeferredEdges.Add(Edge); continue; }
		if (HasSupersedingInput(Edge.Handle) || !IsAbilityInputEdgeCurrent(Edge)) { continue; }
		FGameplayAbilitySpec* Spec = ASC.FindAbilitySpecFromHandle(Edge.Handle);
		if (!Spec || !Spec->Ability || Spec->PendingRemove || Spec->RemoveAfterActivation) { continue; }
		if (Edge.bPressed)
		{
			if (!Spec->IsActive() && Attempted.Contains(Edge.Handle))
			{
				// A later real re-press must not be lost to this batch's one-Try rule.
				DeferredHandles.Add(Edge.Handle);
				DeferredEdges.Add(Edge);
				continue;
			}
			Spec->InputPressed = true;
			if (Spec->IsActive())
			{
				const FGGYGOAbilityStepInputRouter::FDispatch Dispatch{Edge.Handle, Edge.OriginalEvaluationToken, false, ProcessingRevision};
				const FGGYGOAbilityStepInputRouter::FScopedDispatch DispatchScope(*ASC.StepInputRouter, Dispatch);
				ASC.AbilitySpecInputPressed(*Spec); // Real aggregate first edge, never retry.
			}
			else
			{
				TryOnce(Edge.Handle, EAbilityActivationInputOrigin::InitialPress, InitialSourcesByEdge[EdgeIndex], Edge.OriginalEvaluationToken);
			}
		}
		else
		{
			Spec->InputPressed = false;
			if (Spec->IsActive()) { ASC.AbilitySpecInputReleased(*Spec); }
		}
	}
	// Grouped by fixed Spec, so one failed Try can carry several real retry request identities.
	TMap<FGameplayAbilitySpecHandle, TArray<FGGYGOAbilityInputRetryRequest>> RetrySources;
	TArray<FGameplayAbilitySpecHandle> RetryHandles;
	for (const FRetryWork& Work : RetryWork)
	{
		for (const FGameplayAbilitySpecHandle Handle : Work.Handles)
		{
			RetrySources.FindOrAdd(Handle).Add(Work.Request);
			RetryHandles.AddUnique(Handle);
		}
	}
	for (const FGameplayAbilitySpecHandle Handle : RetryHandles)
	{
		if (!CanContinue()) { return; }
		TryOnce(Handle, EAbilityActivationInputOrigin::FiniteRetry, RetrySources.FindChecked(Handle));
	}
	for (const FGameplayAbilitySpecHandle Handle : HeldHandles)
	{
		if (!CanContinue()) { return; }
		const FGameplayAbilitySpec* Spec = ASC.FindAbilitySpecFromHandle(Handle);
		const UGGYGOGameplayAbility* CDO = Spec ? Cast<UGGYGOGameplayAbility>(Spec->Ability) : nullptr;
		if (CDO && CDO->GetActivationPolicy() == EGGYGOAbilityActivationPolicy::WhileInputActive)
		{
			TryOnce(Handle, EAbilityActivationInputOrigin::None, HeldSources.FindChecked(Handle));
		}
	}
	// Held storage is retained until real release/retirement; only these local snapshots expire here.
}

void FGGYGOAbilityInputBuffer::ClearAbilityInput()
{
	check(IsInGameThread());
	TArray<FGameplayAbilitySpecHandle> Affected = InputHeldSpecHandles;
	for (const TPair<uint64, FAbilityInputRequestRecord>& Entry : AbilityInputRequests)
	{
		for (const FGameplayAbilitySpecHandle Handle : Entry.Value.SpecHandles) { Affected.AddUnique(Handle); }
	}
	for (const FAbilityInputEdge& Edge : PendingAbilityInputEdges) { Affected.AddUnique(Edge.Handle); }
	AbilityInputRequests.Reset();
	LastAbilityInputWaitOrder = 0;
	bAwaitingSwitchReturn = false;
	PendingAbilityInputEdges.Reset();
	PendingAbilityInputRetries.Reset();
	InputHeldSpecHandles.Reset();
	++AbilityInputRevision; // 唯一失效方式：旧身份的 Revision 不再匹配；64 位不判耗尽。
	if (CurrentInputAttempt) { CurrentInputAttempt->bClaimed = true; } // 清空后的失败不再按旧来源排队。
	for (const FGameplayAbilitySpecHandle Handle : Affected)
	{
		if (FGameplayAbilitySpec* Spec = ASC.FindAbilitySpecFromHandle(Handle)) { Spec->InputPressed = false; }
	}
	// No native events, delegate dispatch, retry, actor writes or restoration.
}

bool FGGYGOAbilityInputBuffer::IsAbilityActivationFailureOriginCurrent(
	const FAbilityActivationFailureOrigin& Origin) const
{
	check(IsInGameThread());
	if (!IsValid(&ASC) || ASC.HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) { return false; }
	if (Origin.Kind != EAbilityActivationInputOrigin::InitialPress
		&& Origin.Kind != EAbilityActivationInputOrigin::FiniteRetry) { return false; }
	if (ASC.HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked)
		|| !Origin.OwnerActor.IsValid()
		|| !Origin.OwnerActor.HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(ASC.GetOwnerActor()))) { return false; }
	const FGameplayAbilitySpec* Spec = ASC.FindAbilitySpecFromHandle(Origin.Handle);
	const UGGYGOGameplayAbility* CDO = Spec ? Cast<UGGYGOGameplayAbility>(Spec->Ability) : nullptr;
	if (!CDO) { return false; }
	const EGGYGOAbilityActivationPolicy Policy = CDO->GetActivationPolicy();
	const bool bWhileHeld = Policy == EGGYGOAbilityActivationPolicy::WhileInputActive;
	if (Policy != EGGYGOAbilityActivationPolicy::OnInputTriggered && !bWhileHeld) { return false; }
	for (const FGGYGOAbilityInputRetryRequest& Request : Origin.Requests)
	{
		const FAbilityInputRequestRecord* Record = FindAbilityInputRequest(Request.Identity);
		if (Record && RequestMatchesSpec(*Record, Origin.Handle) && (!bWhileHeld || Record->bHeld)
			&& IsBeforeDeadline(*Record)) { return true; }
	}
	return false;
}

FGGYGOAbilityInputBuffer::FScopedAbilityInputActivation::FScopedAbilityInputActivation(
	FGGYGOAbilityInputBuffer& InBuffer, const FAbilityActivationFailureOrigin& Origin)
	: Buffer(InBuffer), Previous(InBuffer.CurrentInputAttempt)
{
	check(IsInGameThread());
	Attempt.Origin = Origin;
	InBuffer.CurrentInputAttempt = &Attempt;
}

FGGYGOAbilityInputBuffer::FScopedAbilityInputActivation::~FScopedAbilityInputActivation()
{
	check(Buffer.CurrentInputAttempt == &Attempt);
	Buffer.CurrentInputAttempt = Previous;
}

void FGGYGOAbilityInputBuffer::NotifyAbilityActivated(FGameplayAbilitySpecHandle Handle)
{
	if (CurrentInputAttempt && CurrentInputAttempt->Origin.Handle == Handle)
	{
		CurrentInputAttempt->bClaimed = true; // 本次输入已激活；之后同 Spec 的失败通知不再排队。
	}
}

FGGYGOAbilityInputBuffer::FQueuedFailure FGGYGOAbilityInputBuffer::ClaimQueuedFailure(
	FGameplayAbilitySpecHandle Handle, const FGameplayTagContainer& FailureTags,
	const TWeakObjectPtr<UGameplayAbility>& FailedAbility)
{
	check(IsInGameThread());
	FQueuedFailure Result;
	// 只有本次输入尝试对应 Spec 的第一次结果属于输入来源；嵌套 Try 的结果不借用它。
	if (!CurrentInputAttempt || CurrentInputAttempt->bClaimed || CurrentInputAttempt->Origin.Handle != Handle)
	{
		return Result;
	}
	CurrentInputAttempt->bClaimed = true;
	Result.Origin = CurrentInputAttempt->Origin;
	const bool bSwitchReturnPending = FailureTags.HasTagExact(GGYGOGameplayTags::Ability_ActivateFail_SwitchReturnPending);
	if (!FailedAbility.IsValid()
		|| !(bSwitchReturnPending || FailureTags.HasTagExact(GGYGOGameplayTags::Ability_ActivateFail_ActivationGroupQueued))
		|| !IsAbilityActivationFailureOriginCurrent(Result.Origin))
	{
		return Result;
	}
	// 列表是本次 Try 冻结的来源，不扫描当前 Tag 或请求。许可只给这次真实 Queued 的 Spec。
	for (const FGGYGOAbilityInputRetryRequest& Request : Result.Origin.Requests)
	{
		const FGameplayAbilitySpec* Spec = ASC.FindAbilitySpecFromHandle(Handle);
		const UGGYGOGameplayAbility* CDO = Spec ? Cast<UGGYGOGameplayAbility>(Spec->Ability) : nullptr;
		if (!CDO || ASC.HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked)) { continue; }
		const bool bWhileHeld = CDO->GetActivationPolicy() == EGGYGOAbilityActivationPolicy::WhileInputActive;
		const FAbilityInputRequestRecord* Live = FindAbilityInputRequest(Request.Identity);
		if (!Live || !RequestMatchesSpec(*Live, Handle) || (bWhileHeld && !Live->bHeld)
			|| !IsBeforeDeadline(*Live)) { continue; }
		FAbilityInputRequestRecord& Record = AbilityInputRequests.FindChecked(Request.Identity.RequestSerial);
		RecordAbilityInputQueuedWait(Record);
		Record.RetryableSpecHandles.AddUnique(Handle);
		Result.Notices.Add(Request);
	}
	bAwaitingSwitchReturn |= bSwitchReturnPending && !Result.Notices.IsEmpty();
	return Result;
}

void FGGYGOAbilityInputBuffer::ReleaseHeldInputForControlLoss()
{
	check(IsInGameThread());
	// 冻结按住集合后逐个释放；回调可以结束能力、改写 Spec 表或输入表，最后统一清空。
	const TArray<FGameplayAbilitySpecHandle> Held = InputHeldSpecHandles;
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(&ASC);
	for (const FGameplayAbilitySpecHandle Handle : Held)
	{
		FGameplayAbilitySpec* Spec = ASC.FindAbilitySpecFromHandle(Handle);
		if (!Spec) { continue; }
		Spec->InputPressed = false;
		if (Spec->IsActive()) { ASC.AbilitySpecInputReleased(*Spec); }
		if (!OriginalASC.IsValid()) { return; }
	}
	ClearAbilityInput();
}

void FGGYGOAbilityInputBuffer::BroadcastQueuedFailure(FGameplayAbilitySpecHandle Handle,
	const FQueuedFailure& Failure, const TWeakObjectPtr<UGameplayAbility>& FailedAbility)
{
	check(IsInGameThread());
	if (Failure.Notices.IsEmpty() || !IsAbilityActivationFailureOriginCurrent(Failure.Origin)) { return; }
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(&ASC);
	for (const FGGYGOAbilityInputRetryRequest& Request : Failure.Notices)
	{
		if (!OriginalASC.IsValid() || !FailedAbility.IsValid()) { break; }
		const FGameplayAbilitySpec* Spec = ASC.FindAbilitySpecFromHandle(Handle);
		const UGGYGOGameplayAbility* CDO = Spec ? Cast<UGGYGOGameplayAbility>(Spec->Ability) : nullptr;
		if (!CDO || ASC.HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked)) { continue; }
		const bool bWhileHeld = CDO->GetActivationPolicy() == EGGYGOAbilityActivationPolicy::WhileInputActive;
		const FAbilityInputRequestRecord* Record = FindAbilityInputRequest(Request.Identity);
		if (!Record || !RequestMatchesSpec(*Record, Handle) || (bWhileHeld && !Record->bHeld)
			|| !Record->RetryableSpecHandles.Contains(Handle) || !IsBeforeDeadline(*Record)) { continue; }
		ASC.OnAbilityInputRetryable.Broadcast(Request);
	}
}

bool FGGYGOAbilityInputBuffer::HasLiveRequestForEdge(const FGGYGOMovementInputEvaluationToken& Edge,
	FGameplayAbilitySpecHandle Spec) const
{
	for (const TPair<uint64, FAbilityInputRequestRecord>& Entry : AbilityInputRequests)
	{
		if (Entry.Value.Request.OriginalEvaluationToken == Edge && RequestMatchesSpec(Entry.Value, Spec))
		{
			return true;
		}
	}
	return false;
}

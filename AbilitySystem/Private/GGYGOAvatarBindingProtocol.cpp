/**
 * @file GGYGOAvatarBindingProtocol.cpp
 * @brief Single-owner transitions of ASC binding metadata; no native execution or external callbacks.
 */
#include "AbilitySystem/Private/GGYGOAvatarBindingProtocol.h"

bool FGGYGOAvatarBindingProtocol::IsInvalidationReason(EGGYGOAvatarBindingReason Reason)
{
	switch (Reason)
	{
	case EGGYGOAvatarBindingReason::LifecycleClosed:
	case EGGYGOAvatarBindingReason::ActorInfoMismatch:
	case EGGYGOAvatarBindingReason::OperationInvalidated:
	case EGGYGOAvatarBindingReason::RequestContextExpired:
		return true;
	default:
		return false;
	}
}

bool FGGYGOAvatarBindingProtocol::IsActorInfoKind(EGGYGOAvatarBindingKind Kind)
{
	return Kind == EGGYGOAvatarBindingKind::Init
		|| Kind == EGGYGOAvatarBindingKind::Clear
		|| Kind == EGGYGOAvatarBindingKind::Refresh;
}

bool FGGYGOAvatarBindingProtocol::InvalidateBinding(
	const FGGYGOAvatarBindingContext& Expected, EGGYGOAvatarBindingReason Reason,
	EGGYGOAvatarBindingReason& OutRejectionReason)
{
	OutRejectionReason = EGGYGOAvatarBindingReason::InvalidRequest;
	check(IsInGameThread());
	if (!IsInvalidationReason(Reason) || !Expected.HasIssuedContext())
	{
		return false;
	}
	if (!Context.HasSameContext(Expected))
	{
		OutRejectionReason = EGGYGOAvatarBindingReason::ExpectedContextMismatch;
		return false;
	}
	IdentityState = EIdentityState::Revoked;
	// Logical revocation retains exact committed cleanup provenance, never work/Ready permission.
	ReleasePublicationForContext(Expected);
	// Deliberately retain the pending operation: its own ID is required to revoke it.
	OutRejectionReason = EGGYGOAvatarBindingReason::None;
	return true;
}

void FGGYGOAvatarBindingProtocol::DiscardOperation(bool bRevokeBeforeContext)
{
	check(IsInGameThread());
	if (bRevokeBeforeContext
		&& IsActorInfoKind(PendingOperation.Kind)
		&& Context.HasSameContext(PendingOperation.BeforeContext))
	{
		IdentityState = EIdentityState::Revoked;
		// A pre-write failure keeps old cleanup provenance; actual writes already retired it.
		ReleasePublicationForContext(PendingOperation.BeforeContext);
	}
	PendingOperation = {};
}

bool FGGYGOAvatarBindingProtocol::InvalidateOperation(
	const FGGYGOAvatarBindingOperationIdentity& ExpectedOperation, EGGYGOAvatarBindingReason Reason)
{
	check(IsInGameThread());
	if (!IsInvalidationReason(Reason)
		|| !PendingOperation.Identity.HasSameIdentity(ExpectedOperation))
	{
		return false;
	}
	DiscardOperation(/*bRevokeBeforeContext=*/true);
	return true;
}

void FGGYGOAvatarBindingProtocol::ReleasePublicationForContext(
	const FGGYGOAvatarBindingContext& Expected)
{
	check(IsInGameThread());
	if (PublicationRecord.Context.HasSameContext(Expected))
	{
		// This also closes successful Consumed metadata after its strong proof was released.
		PublicationRecord.Publication = FGGYGOAvatarBindingPublicationReceipt{};
		PublicationRecord.Phase = EPublicationPhase::Closed;
	}
}

bool FGGYGOAvatarBindingProtocol::ReserveValidatedOperation(UGGYGOAbilitySystemComponent* Issuer,
	FIdentityOperation Candidate, FGGYGOAvatarBindingOperationIdentity& OutOperation,
	EGGYGOAvatarBindingReason& OutReason)
{
	check(IsInGameThread());
	if (LastIssuedSerial == MAX_uint64)
	{
		OutReason = EGGYGOAvatarBindingReason::SerialExhausted;
		return false;
	}

	Candidate.Identity.Issuer = Issuer;
	Candidate.Identity.Serial = ++LastIssuedSerial;
	PendingOperation = MoveTemp(Candidate);
	OutOperation = PendingOperation.Identity;
	OutReason = EGGYGOAvatarBindingReason::None;
	return true;
}

bool FGGYGOAvatarBindingProtocol::CheckCommitOperation(
	const FGGYGOAvatarBindingOperationIdentity& Operation, EGGYGOAvatarBindingReason& OutReason)
{
	OutReason = EGGYGOAvatarBindingReason::OperationInvalidated;
	check(IsInGameThread());
	if (!PendingOperation.Identity.HasSameIdentity(Operation))
	{
		return false;
	}
	const auto RejectCommit = [this, &OutReason](EGGYGOAvatarBindingReason Reason)
	{
		OutReason = Reason;
		DiscardOperation(/*bRevokeBeforeContext=*/true);
		return false;
	};
	const FIdentityOperation& Pending = PendingOperation;
	if (!IsActorInfoKind(Pending.Kind))
	{
		return RejectCommit(EGGYGOAvatarBindingReason::InvalidRequest);
	}
	if (Pending.Admission == EAdmission::BootstrapNeverCommitted)
	{
		if (IdentityState != EIdentityState::Unissued
			|| Context.HasIssuedContext())
		{
			return RejectCommit(EGGYGOAvatarBindingReason::ExpectedContextMismatch);
		}
	}
	else if (!Context.HasSameContext(Pending.BeforeContext)
		|| (Pending.Kind == EGGYGOAvatarBindingKind::Refresh
			&& IdentityState != EIdentityState::Current))
	{
		return RejectCommit(EGGYGOAvatarBindingReason::OperationInvalidated);
	}

	return true;
}

bool FGGYGOAvatarBindingProtocol::CommitValidatedSource(
	const FGGYGOAvatarBindingOperationIdentity& Operation, FActorInfoSnapshot Actual,
	FGGYGOAvatarBindingContext& OutCommittedContext, EGGYGOAvatarBindingReason& OutReason)
{
	check(IsInGameThread());
	check(PendingOperation.Identity.HasSameIdentity(Operation));
	const FIdentityOperation& Pending = PendingOperation;
	const auto RejectCommit = [this, &OutReason](EGGYGOAvatarBindingReason Reason)
	{
		OutReason = Reason;
		DiscardOperation(/*bRevokeBeforeContext=*/true);
		return false;
	};
	if (Actual.Allocation.Get() != Pending.BeforeActual.Allocation.Get()
		|| !Actual.Source.OwnerActor.HasSameIndexAndSerialNumber(Pending.ExpectedOwnerActor)
		|| !Actual.Source.AvatarActor.HasSameIndexAndSerialNumber(Pending.ExpectedAvatarActor)
		|| ((Pending.Kind == EGGYGOAvatarBindingKind::Init || Pending.Kind == EGGYGOAvatarBindingKind::Refresh)
			&& Actual.Source.ActorInfoAffectedAnimInstanceTag != Actual.Source.ASCAffectedAnimInstanceTag))
	{
		return RejectCommit(EGGYGOAvatarBindingReason::ActorInfoMismatch);
	}

	FGGYGOAvatarBindingContext Committed;
	Committed.Binding = Pending.Kind == EGGYGOAvatarBindingKind::Refresh
		? Context.Binding : FGGYGOAvatarBindingIdentity{};
	if (Pending.Kind != EGGYGOAvatarBindingKind::Refresh)
	{
		Committed.Binding.Issuer = Operation.Issuer;
		Committed.Binding.Serial = Operation.Serial;
	}
	Committed.LastActorInfoWrite = Operation;
	Context = Committed;
	CommittedSource = MoveTemp(Actual);
	IdentityState = EIdentityState::Current;
	DiscardOperation(/*bRevokeBeforeContext=*/false);
	OutCommittedContext = Committed;
	OutReason = EGGYGOAvatarBindingReason::None;
	return true;
}

bool FGGYGOAvatarBindingProtocol::TryEnterNativeWrite()
{
	check(IsInGameThread());
	if (bNativeWriteBusy) { return false; }
	bNativeWriteBusy = true;
	return true;
}

void FGGYGOAvatarBindingProtocol::LeaveNativeWrite()
{
	check(IsInGameThread());
	bNativeWriteBusy = false;
}

void FGGYGOAvatarBindingProtocol::RetireCommittedSource()
{
	check(IsInGameThread());
	CommittedSource = {};
}

void FGGYGOAvatarBindingProtocol::RetainValidatedFailedInitCleanup(
	const TSharedPtr<const FFailedInitCleanupProof>& Original)
{
	check(IsInGameThread());
	FailedInitCleanup = Original;
}

void FGGYGOAvatarBindingProtocol::RetireFailedInitCleanup()
{
	check(IsInGameThread());
	FailedInitCleanup.Reset();
}

void FGGYGOAvatarBindingProtocol::InstallCommittedPublication(
	const FGGYGOAvatarBindingPublicationReceipt& Publication, const FGGYGOAvatarBindingContext& Committed)
{
	check(IsInGameThread());
	PublicationRecord = {Publication, Committed, EPublicationPhase::Pending};
}

void FGGYGOAvatarBindingProtocol::ClosePublicationIfMatching(
	const FGGYGOAvatarBindingPublicationReceipt& Publication, const FGGYGOAvatarBindingContext& Committed)
{
	check(IsInGameThread());
	if (Publication.Proof.IsValid() && PublicationRecord.Publication.Proof == Publication.Proof
		&& PublicationRecord.Context.HasSameContext(Committed)
		&& (PublicationRecord.Phase == EPublicationPhase::Pending
			|| PublicationRecord.Phase == EPublicationPhase::Dispatching))
	{
		PublicationRecord.Publication = FGGYGOAvatarBindingPublicationReceipt{};
		PublicationRecord.Phase = EPublicationPhase::Closed;
	}
}

void FGGYGOAvatarBindingProtocol::BeginValidatedPublicationDispatch()
{
	check(IsInGameThread());
	PublicationRecord.Phase = EPublicationPhase::Dispatching;
}

void FGGYGOAvatarBindingProtocol::ConsumeValidatedPublication()
{
	check(IsInGameThread());
	PublicationRecord.Phase = EPublicationPhase::Consumed;
	PublicationRecord.Publication = FGGYGOAvatarBindingPublicationReceipt{};
}

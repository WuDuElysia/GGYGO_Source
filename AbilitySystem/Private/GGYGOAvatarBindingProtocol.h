/**
 * @file GGYGOAvatarBindingProtocol.h
 * @brief ASC-owned binding metadata and exact operation/publication transitions.
 */
#pragma once

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"

/**
 * One private resource of one ASC. Owns identity issuance, pending operation, committed
 * source, publication consumption and failed-write cleanup provenance.
 * Takes already-validated stack values; never captures native state, invokes callers,
 * performs ActorInfo writes, retains predicates, or schedules gameplay work.
 */
class FGGYGOAvatarBindingProtocol final
{
public:
	using EIdentityState = UGGYGOAbilitySystemComponent::EAvatarBindingIdentityState;
	using EAdmission = UGGYGOAbilitySystemComponent::EAvatarBindingIdentityAdmission;
	using EPublicationPhase = UGGYGOAbilitySystemComponent::EAvatarBindingPublicationPhase;
	using FActorInfoSnapshot = UGGYGOAbilitySystemComponent::FActualAvatarBindingActorInfoSnapshot;
	using FIdentityOperation = UGGYGOAbilitySystemComponent::FAvatarBindingIdentityOperation;
	using FFailedInitCleanupProof = UGGYGOAbilitySystemComponent::FFailedAvatarActorInfoInitCleanupProof;
	using FPublicationRecord = UGGYGOAbilitySystemComponent::FAvatarBindingPublicationRecord;

	FGGYGOAvatarBindingProtocol() = default;
	FGGYGOAvatarBindingProtocol(const FGGYGOAvatarBindingProtocol&) = delete;
	FGGYGOAvatarBindingProtocol& operator=(const FGGYGOAvatarBindingProtocol&) = delete;

	/** One pure classification for native ASC admission and metadata transitions. */
	static bool IsActorInfoKind(EGGYGOAvatarBindingKind Kind);

	const FGGYGOAvatarBindingContext& GetContext() const { return Context; }
	EIdentityState GetIdentityState() const { return IdentityState; }
	const FActorInfoSnapshot& GetCommittedSource() const { return CommittedSource; }
	const FIdentityOperation& GetOperation() const { return PendingOperation; }
	const FPublicationRecord& GetPublication() const { return PublicationRecord; }
	const TSharedPtr<const FFailedInitCleanupProof>& GetFailedInitCleanup() const { return FailedInitCleanup; }
	bool IsNativeWriteBusy() const { return bNativeWriteBusy; }

	/** All caller/source admission is complete; issue exactly one identity, without wrap. */
	bool ReserveValidatedOperation(UGGYGOAbilitySystemComponent* Issuer, FIdentityOperation Candidate,
		FGGYGOAvatarBindingOperationIdentity& OutOperation, EGGYGOAvatarBindingReason& OutReason);
	/** Metadata admission before ASC captures the returned native ActorInfo. */
	bool CheckCommitOperation(const FGGYGOAvatarBindingOperationIdentity& Operation,
		EGGYGOAvatarBindingReason& OutReason);
	/** Source was validated by ASC on this stack; endpoint/allocation coherence and commit are atomic here. */
	bool CommitValidatedSource(const FGGYGOAvatarBindingOperationIdentity& Operation,
		FActorInfoSnapshot Actual, FGGYGOAvatarBindingContext& OutCommittedContext,
		EGGYGOAvatarBindingReason& OutReason);
	void DiscardOperation(bool bRevokeBeforeContext);
	bool InvalidateOperation(const FGGYGOAvatarBindingOperationIdentity& ExpectedOperation,
		EGGYGOAvatarBindingReason Reason);
	bool InvalidateBinding(const FGGYGOAvatarBindingContext& Expected, EGGYGOAvatarBindingReason Reason,
		EGGYGOAvatarBindingReason& OutRejectionReason);

	bool TryEnterNativeWrite();
	void LeaveNativeWrite();
	void RetireCommittedSource();
	void RetainValidatedFailedInitCleanup(const TSharedPtr<const FFailedInitCleanupProof>& Original);
	void RetireFailedInitCleanup();

	void InstallCommittedPublication(const FGGYGOAvatarBindingPublicationReceipt& Publication,
		const FGGYGOAvatarBindingContext& Committed);
	void ReleasePublicationForContext(const FGGYGOAvatarBindingContext& Expected);
	void ClosePublicationIfMatching(const FGGYGOAvatarBindingPublicationReceipt& Publication,
		const FGGYGOAvatarBindingContext& Committed);
	/** ASC just revalidated this exact Pending publication; no external call before transition. */
	void BeginValidatedPublicationDispatch();
	/** ASC just revalidated this exact Dispatching publication; release its proof after consumption. */
	void ConsumeValidatedPublication();

private:
	static bool IsInvalidationReason(EGGYGOAvatarBindingReason Reason);

	uint64 LastIssuedSerial = 0;
	FGGYGOAvatarBindingContext Context;
	EIdentityState IdentityState = EIdentityState::Unissued;
	FActorInfoSnapshot CommittedSource;
	FIdentityOperation PendingOperation;
	TSharedPtr<const FFailedInitCleanupProof> FailedInitCleanup;
	bool bNativeWriteBusy = false;
	FPublicationRecord PublicationRecord;
};

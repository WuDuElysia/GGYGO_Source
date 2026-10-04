#pragma once

#include "CoreTypes.h"
#include "Templates/Function.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

class AActor;
class UGGYGOAbilitySystemComponent;

enum class EGGYGOAvatarBindingKind : uint8
{
	Invalid = 0,
	Init,
	Clear,
	Refresh,
	CancelAbilities,
	RemoveGameplayCues
};

enum class EGGYGOAvatarBindingOutcome : uint8
{
	Succeeded = 0,
	Rejected,
	Busy,
	Stale,
	Failed
};

enum class EGGYGOAvatarBindingReason : uint8
{
	None = 0,
	InvalidRequest,
	MissingContextQuery,
	InvalidASC,
	InvalidActorInfo,
	InvalidOwner,
	InvalidAvatar,
	ExpectedContextMismatch,
	RequestContextExpired,
	LifecycleClosed,
	ActorInfoMismatch,
	NativeWriteBusy,
	OperationInvalidated,
	SerialExhausted,
	/** Publication admission only; these do not authorize binding invalidation. */
	InvalidPublication,
	PublicationInProgress,
	PublicationAlreadyConsumed
};

enum class EGGYGOAvatarBindingNoticeKind : uint8
{
	Invalid = 0,
	Initialized,
	Released,
	Refreshed,
	Invalidated
};

enum class EGGYGOAvatarBindingClearMode : uint8
{
	None = 0,
	PreserveOwner,
	ClearActorInfo
};

/**
 * ASC-issued binding identity.
 * Serial zero is invalid. Nonzero serials are never reused or wrapped.
 * The issuer uses one private serial allocator for binding and operation IDs.
 * Exhaustion rejects before native mutation; it never substitutes an ID.
 */
struct FGGYGOAvatarBindingIdentity
{
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> Issuer{};
	uint64 Serial = 0;

	/** Issued value only; does not prove issuer liveness or current ownership. */
	bool HasIssuedIdentity() const
	{
		return Serial != 0 && !Issuer.IsExplicitlyNull();
	}

	/** Compare retained weak identity, including an expired issuer. */
	bool HasSameIdentity(const FGGYGOAvatarBindingIdentity& Other) const
	{
		return HasIssuedIdentity()
			&& Other.HasIssuedIdentity()
			&& Serial == Other.Serial
			&& Issuer.HasSameIndexAndSerialNumber(Other.Issuer);
	}
};

/** Distinct value type: an operation ID cannot be used as a binding ID. */
struct FGGYGOAvatarBindingOperationIdentity
{
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> Issuer{};
	uint64 Serial = 0;

	bool HasIssuedIdentity() const
	{
		return Serial != 0 && !Issuer.IsExplicitlyNull();
	}

	bool HasSameIdentity(
		const FGGYGOAvatarBindingOperationIdentity& Other) const
	{
		return HasIssuedIdentity()
			&& Other.HasIssuedIdentity()
			&& Serial == Other.Serial
			&& Issuer.HasSameIndexAndSerialNumber(Other.Issuer);
	}
};

/**
 * Init/Clear commit a new Binding and LastActorInfoWrite.
 * Refresh preserves Binding and commits a new LastActorInfoWrite.
 * Cancel/Cue operations do not advance either field.
 * This is identity evidence, not a second ActorInfo or Avatar authority.
 */
struct FGGYGOAvatarBindingContext
{
	FGGYGOAvatarBindingIdentity Binding{};
	FGGYGOAvatarBindingOperationIdentity LastActorInfoWrite{};

	/** Value consistency only; no ActorInfo or lifecycle validation. */
	bool HasIssuedContext() const
	{
		return Binding.HasIssuedIdentity()
			&& LastActorInfoWrite.HasIssuedIdentity()
			&& Binding.Issuer.HasSameIndexAndSerialNumber(
				LastActorInfoWrite.Issuer);
	}

	bool HasSameContext(const FGGYGOAvatarBindingContext& Other) const
	{
		return HasIssuedContext()
			&& Other.HasIssuedContext()
			&& Binding.HasSameIdentity(Other.Binding)
			&& LastActorInfoWrite.HasSameIdentity(
				Other.LastActorInfoWrite);
	}
};

/**
 * Synchronous request only. Invalid/default values grant no permission.
 *
 * Init uses OwnerActor and AvatarActor; OwnerActor is required.
 * A null Init AvatarActor explicitly requests an owner-only context.
 * Other kinds require both request actor fields to be explicitly null;
 * their original endpoints come from ExpectedContext and ASC validation.
 *
 * Clear requires an explicit non-None ClearMode.
 * Other kinds require ClearMode == None.
 * Cancel filters remain arguments of the later ASC API, outside this header.
 */
struct FGGYGOAvatarBindingRequest
{
	EGGYGOAvatarBindingKind Kind = EGGYGOAvatarBindingKind::Invalid;
	FGGYGOAvatarBindingContext ExpectedContext{};
	TWeakObjectPtr<AActor> OwnerActor{};
	TWeakObjectPtr<AActor> AvatarActor{};
	EGGYGOAvatarBindingClearMode ClearMode =
		EGGYGOAvatarBindingClearMode::None;

	/**
	 * Required, side-effect-free, game-thread query.
	 *
	 * Exact signature: bool().
	 * The caller supplies a stack-local request whose storage and borrowed
	 * captures survive the entire synchronous ASC call, including reentry.
	 * TFunction owns its closure; it does not extend referenced lifetimes.
	 *
	 * ASC may invoke the query repeatedly before/after external calls.
	 * ASC must not retain it, queue it, or invoke it after returning.
	 * Do not mutate, broadcast, bind, play, or initiate cleanup here.
	 *
	 * A true result is only caller-context evidence. Captured pointers,
	 * weak references and this predicate do not establish authority.
	 * ASC independently rechecks identities, actual fields and lifecycle.
	 *
	 * An unbound query is Rejected/MissingContextQuery before native entry.
	 * False is Stale/RequestContextExpired and grants no successful result.
	 */
	TFunction<bool()> IsRequestContextCurrent{};
};

/**
 * Operation-local evidence, copied by value.
 * CommittedContext records this operation's commit, never a successor.
 */
struct FGGYGOAvatarBindingResult
{
	EGGYGOAvatarBindingOutcome Outcome =
		EGGYGOAvatarBindingOutcome::Rejected;
	EGGYGOAvatarBindingReason Reason =
		EGGYGOAvatarBindingReason::InvalidRequest;

	FGGYGOAvatarBindingOperationIdentity Operation{};
	FGGYGOAvatarBindingContext Before{};
	FGGYGOAvatarBindingContext CommittedContext{};

	/**
	 * True only after this Init/Clear/Refresh committed.
	 * It remains true if a completion callback later makes Outcome Stale.
	 * Cancel/Cue success leaves it false and proves no deferred GA completion.
	 */
	bool bCommitted = false;
};

/**
 * Copied notification payload; never resolve ownership from a later cache.
 * Initialized/Refreshed actor fields describe After.
 * Released/Invalidated actor fields describe the affected Before resources.
 * Actor fields are descriptive weak references, not authority credentials.
 */
struct FGGYGOAvatarBindingNotice
{
	EGGYGOAvatarBindingNoticeKind Kind =
		EGGYGOAvatarBindingNoticeKind::Invalid;

	FGGYGOAvatarBindingOperationIdentity Operation{};
	FGGYGOAvatarBindingContext Before{};
	FGGYGOAvatarBindingContext After{};
	TWeakObjectPtr<AActor> OwnerActor{};
	TWeakObjectPtr<AActor> AvatarActor{};
};

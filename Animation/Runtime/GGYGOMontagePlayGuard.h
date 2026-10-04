// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UAnimInstance;
class UAnimMontage;
class UGGYGOMontageGuardAnimInstance;

/** Outcome of one scoped call. A rejected or failed child is not a successful takeover. */
enum class EGGYGOMontagePlayGuardOutcome : uint8
{
	/** No native play has run for this call. Also the initial, unfinished outcome. */
	NotExecuted,
	/** This call created the exact surviving instance and passed the caller validity checks. */
	Accepted,
	/** Native play ran but did not produce an accepted instance. */
	Failed,
	/** A successful descendant took over before this native play returned. */
	Superseded,
	/** Reentry was rejected before this call could create a montage instance. */
	PrecreationRejected,
	/** The innermost enclosing scope had already returned from native play. No child play ran. */
	PostWriteRejected,
	/** The original instance or entry path does not provide the scoped guard contract. */
	Unsupported,
	/** The captured animation lifecycle or caller context is no longer current. */
	LifecycleInvalid
};

/** Native animation stage, independent of the longer caller scope and its GAS writeback. */
enum class EGGYGOMontagePlayGuardNativeStage : uint8
{
	NotEntered,
	Executing,
	Returned
};

/** Copied identity only; neither an asset pointer nor an 8-bit GAS play counter is an instance ID. */
struct FGGYGOMontagePlayGuardIdentity
{
	TWeakObjectPtr<UAnimInstance> OriginalAnimInstance;
	/** Issued by the guard; invalidated on initialize/uninitialize/owner replacement. Zero is invalid. */
	uint64 LifecycleGeneration = 0;
	/** Issued by the guard, never reused across lifecycle resets; exhaustion rejects, never wraps. */
	uint64 CallId = 0;
	/** This call's engine instance ID, copied while valid; INDEX_NONE until creation is observed. */
	int32 CreatedInstanceId = INDEX_NONE;
};

/** Caller inputs. No GAS types or authority are stored here. */
struct FGGYGOMontagePlayGuardRequest
{
	/** Capture the actual instance before entering the caller's native play function. */
	TWeakObjectPtr<UAnimInstance> OriginalAnimInstance;
	TWeakObjectPtr<UAnimMontage> RequestedMontage;
	/** Opaque caller/coordinator identity (for example an ASC), never interpreted as a GAS type. */
	TWeakObjectPtr<UObject> CallerIdentity;
	/**
	 * Required, side-effect-free game-thread query. Capture the original caller/Avatar/activation
	 * context; return false after replacement, cancellation or loss of that context. Do not invoke
	 * play, broadcast events or switch ownership here. References captured by it must outlive scope.
	 */
	TFunction<bool()> IsCallerContextCurrent;
};

/** Read-only result snapshot. It does not own an instance or mirror Local/Rep/GA montage state. */
struct FGGYGOMontagePlayGuardResult
{
	FGGYGOMontagePlayGuardIdentity Identity;
	EGGYGOMontagePlayGuardOutcome Outcome = EGGYGOMontagePlayGuardOutcome::NotExecuted;
	EGGYGOMontagePlayGuardNativeStage NativeStage = EGGYGOMontagePlayGuardNativeStage::NotEntered;
	/** Successful descendant's call ID on the same original instance; zero means no takeover. */
	uint64 SupersedingCallId = 0;
	/** Animation barrier return, before caller writeback. Only Accepted may expose a positive value. */
	float GuardedNativeReturnValue = 0.0f;
	/** Caller return supplied to Complete, for evidence; it cannot promote a rejected call to Accepted. */
	float CallerReturnValue = 0.0f;
};

/**
 * Game-thread-only, noncopyable/nonmovable caller-stack scope. It must span the ENTIRE caller
 * Super::PlayMontage invocation, including Local/Rep/GA writeback, section jump and prediction
 * binding. Construct -> CanExecute -> caller Super -> Complete -> copy result -> destruct.
 * If CanExecute is false, skip Super and Complete(0). Never retain GetResult's reference after
 * destruction. All methods below are declarations only; definitions await A2 authorization.
 *
 * The thin Animation guard is the sole scope-chain owner for each original AnimInstance.
 * Registration captures its lifecycle generation and issues the nonreusing call ID; caller input
 * cannot manufacture either. The chain has nonowning pointers to live caller scopes, no global
 * attempt map, timer, scheduler or second montage executor. Destruction removes only this call's
 * registration on the original instance. It must never stop a montage on the current/new owner.
 *
 * Reentry classification uses the innermost scope's native stage. NotEntered/Returned reentry is
 * rejected before child native play; an older Executing ancestor does not bypass that rule.
 * Executing reentry before the parent's exact instance is observed is PrecreationRejected, too.
 * An Executing scope with that instance may admit supported reentry; only acceptance supersedes
 * ancestors. A failed/rejected child alone never does. Exact engine IDs must be re-resolved after
 * callbacks; no raw FAnimMontageInstance pointer survives a callback in this API.
 *
 * Lifecycle invalidation must invalidate live registrations without freeing caller-stack storage.
 * Complete seals evidence after caller return; it does not roll back GAS writes. The native
 * barrier must classify and suppress a rejected/superseded positive return BEFORE caller writes.
 * Destruction is idempotent with Complete and invalidation, including a missing original object.
 * No asset migration, fractional-loop or simulated-play support is implied by this declaration.
 */
class GGYGO_API FGGYGOMontagePlayGuardScope final
{
public:
	explicit FGGYGOMontagePlayGuardScope(FGGYGOMontagePlayGuardRequest&& InRequest);
	~FGGYGOMontagePlayGuardScope();

	FGGYGOMontagePlayGuardScope(const FGGYGOMontagePlayGuardScope&) = delete;
	FGGYGOMontagePlayGuardScope& operator=(const FGGYGOMontagePlayGuardScope&) = delete;
	FGGYGOMontagePlayGuardScope(FGGYGOMontagePlayGuardScope&&) = delete;
	FGGYGOMontagePlayGuardScope& operator=(FGGYGOMontagePlayGuardScope&&) = delete;

	/** Recheck validity immediately before caller Super. True grants entry, not successful playback. */
	bool CanExecute();
	/** Seal once after caller return (or skipped entry). Repeated calls return the first sealed result. */
	const FGGYGOMontagePlayGuardResult& Complete(float InCallerReturnValue);
	/** Scope-owned evidence. Copy after Complete; observers have no mutation access. */
	const FGGYGOMontagePlayGuardResult& GetResult() const;

private:
	friend class UGGYGOMontageGuardAnimInstance;
	struct FState;
	TUniquePtr<FState> State;
};

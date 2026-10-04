#pragma once

#include "Animation/Runtime/GGYGOMontagePlayGuard.h"
#include "Templates/SharedPointer.h"

class UGGYGOAbilitySystemComponent;

/** Copied original resource only. Only ASC may issue its immutable native-write proof. */
class GGYGO_API FGGYGOAbilityMontagePlaybackHandle final
{
public:
	FGGYGOAbilityMontagePlaybackHandle() = default;
	bool HasPlayback() const { return Proof.IsValid(); }
	bool HasSamePlayback(const FGGYGOAbilityMontagePlaybackHandle& Other) const
	{
		return Proof.IsValid() && Proof == Other.Proof;
	}

private:
	struct FPlaybackProof;
	TSharedPtr<const FPlaybackProof> Proof{};
	friend class UGGYGOAbilitySystemComponent;
};

enum class EGGYGOAbilityMontagePlaybackOutcome : uint8
{
	Rejected = 0,
	Succeeded,
	NoOwnedPlayback,
	Busy,
	Stale,
	Failed
};

enum class EGGYGOAbilityMontagePlaybackReason : uint8
{
	InvalidRequest = 0,
	None,
	MissingCallerQuery,
	InvalidASC,
	InvalidAbility,
	InvalidActorInfo,
	NoOwnedPlayback,
	WrongIssuer,
	PlaybackReplaced,
	ActorInfoChanged,
	AbilitySourceChanged,
	GuardLifecycleInvalid,
	GuardUnsupported,
	GuardRejected,
	GuardSuperseded,
	NativePlayFailed,
	LocalWriteMismatch,
	NativeWriteBusy
};

/** Native history is preserved on failure. Only Succeeded may issue Playback. */
struct FGGYGOAbilityMontagePlaybackResult
{
	EGGYGOAbilityMontagePlaybackOutcome Outcome = EGGYGOAbilityMontagePlaybackOutcome::Rejected;
	EGGYGOAbilityMontagePlaybackReason Reason = EGGYGOAbilityMontagePlaybackReason::InvalidRequest;
	FGGYGOMontagePlayGuardResult Guard{};
	float Duration = 0.0f;
	FGGYGOAbilityMontagePlaybackHandle Playback{};
};

/** Pure query: copied original evidence does not grant a new playback or change state. */
struct FGGYGOAbilityMontageOwnershipCheck
{
	EGGYGOAbilityMontagePlaybackOutcome Outcome = EGGYGOAbilityMontagePlaybackOutcome::Rejected;
	EGGYGOAbilityMontagePlaybackReason Reason = EGGYGOAbilityMontagePlaybackReason::InvalidRequest;
	FGGYGOMontagePlayGuardResult Guard{};
	FGGYGOAbilityMontagePlaybackHandle Original{};
};

/** Succeeded proves only the native GA/Local animating-pointer clear, never a Montage stop. */
struct FGGYGOAbilityMontageClearResult
{
	EGGYGOAbilityMontagePlaybackOutcome Outcome = EGGYGOAbilityMontagePlaybackOutcome::Rejected;
	EGGYGOAbilityMontagePlaybackReason Reason = EGGYGOAbilityMontagePlaybackReason::InvalidRequest;
	FGGYGOMontagePlayGuardResult Guard{};
	FGGYGOAbilityMontagePlaybackHandle Original{};
	bool bCleared = false;
};

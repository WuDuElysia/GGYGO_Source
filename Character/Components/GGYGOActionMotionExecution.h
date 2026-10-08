/**
 * @file GGYGOActionMotionExecution.h
 * @brief CMC-owned action slot, original resources and native completion/failure observers.
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/Data/GGYGOActionMotionSourceBinding.h"
#include "UObject/StrongObjectPtr.h"

class UGGYGOCharacterMovementComponent;
class UGGYGOActionMotionProfile;
class ACharacter;
class USkeletalMeshComponent;
class UAnimInstance;
struct FRootMotionSource;
struct FRootMotionSource_GGYGOActionCurve;
struct FGGYGOQualifiedMovementIntent;

enum class EGGYGOActionMotionReleaseReason : uint8
{
	Completed, Cancelled, Replaced, OwnerInvalidated
};

DECLARE_DELEGATE_TwoParams(FGGYGOActionMotionFailureDelegate, int32, const FString&);
DECLARE_DELEGATE_OneParam(FGGYGOActionMotionCompletionDelegate, int32);

/** Immutable original action identity; native RMS owns time and per-move release state. */
struct FGGYGOActionMotionResource
{
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> Owner;
	TWeakObjectPtr<ACharacter> Character;
	TWeakObjectPtr<USkeletalMeshComponent> Mesh;
	TWeakObjectPtr<UAnimInstance> AnimInstance;
	FGGYGOActionMotionSourceBindingPtr Source;
	/** Only the explicitly selected Profile mode retains this original configuration. */
	TStrongObjectPtr<const UGGYGOActionMotionProfile> Profile;
	int32 MontageInstanceId = INDEX_NONE;
	int32 Handle = INDEX_NONE;
	float InstancePlayRate = 0.0f;
};

/** Owns one action execution slot; native RMS alone owns time and movement physics. */
class GGYGO_API FGGYGOActionMotionExecution
{
public:
	explicit FGGYGOActionMotionExecution(UGGYGOCharacterMovementComponent& InMovement) : Movement(InMovement) {}
	FGGYGOActionMotionExecution(const FGGYGOActionMotionExecution&) = delete;
	FGGYGOActionMotionExecution& operator=(const FGGYGOActionMotionExecution&) = delete;
	static constexpr uint16 SourcePriority = 100;
	static const FName& SourceName();
	static bool IsRegisteredSource(const FRootMotionSource& Source);

private:
	friend class UGGYGOCharacterMovementComponent;
	/** Keep detached delegate captures alive until the enclosing CMC/action operation returns. */
	struct FRetiredObservers
	{
		FGGYGOActionMotionFailureDelegate Failure;
		FGGYGOActionMotionCompletionDelegate Completion;
	};
	FRetiredObservers RetireCompletedResource();
	FRetiredObservers Close();
	bool HasMontageResource() const { return ActiveMontageActionResource.IsValid() || CompletedMontageActionResource.IsValid(); }
	bool HasOwnedSlot() const { return ActiveActionMotionHandle != INDEX_NONE; }
	int32 GetLastRetiredMontageHandle() const { return LastRetiredMontageActionHandle; }
	void ResetSkippedInterval() { ActionSkippedMovementTickTime = 0.0f; }
	void CaptureSkippedInterval(float TickTime) { ActionSkippedMovementTickTime = TickTime; }

	bool IsMontageActionMovementModeSupported() const;
	bool BeginMontageActionMotion(const FGGYGOActionMotionSourceBindingPtr& OriginalSource,
		int32 OriginalMontageInstanceId, float MontagePositionSeconds, float EffectiveMontagePlayRate,
		float TranslationScale, int32& OutHandle, FString& OutError);
	bool ValidateMontageActionRuntime(const FRootMotionSource_GGYGOActionCurve& Source,
		float SimulationTime, FString& OutError) const;
	bool NotifyMontageActionNaturalBlendOut(int32 OriginalHandle, int32 OriginalMontageInstanceId, FString& OutError);
	bool ObserveMontageActionMotionCompletion(int32 OriginalHandle,
		FGGYGOActionMotionCompletionDelegate Callback, FString& OutError);
	void PublishMontageActionMotionCompletion();
	bool ObserveActionMotionFailure(int32 OriginalHandle,
		FGGYGOActionMotionFailureDelegate Callback, FString& OutError);
	bool ValidateProfileActionRuntime(const FRootMotionSource_GGYGOActionCurve& Source, FString& OutError) const;
	void FailProfileActionMotion(const TSharedPtr<const FGGYGOActionMotionResource>& OriginalResource, const FString& Error);
	bool ObserveMontageActionMotionFailure(int32 OriginalHandle,
		FGGYGOActionMotionFailureDelegate Callback, FString& OutError);
	void FailMontageActionMotion(const TSharedPtr<const FGGYGOActionMotionResource>& OriginalResource, const FString& Error);
	void NeutralizeActionSource(const TSharedPtr<const FGGYGOActionMotionResource>& OriginalResource);
	bool ReleaseMontageActionMotion(int32 OriginalHandle, EGGYGOActionMotionReleaseReason Reason, FString& OutError);
	bool CancelMontageActionMotionForMovement(int32 OriginalHandle,
		const FGGYGOQualifiedMovementIntent& OriginalIntent, FString& OutError);
	void ResumeLocomotionAfterAction();
	int32 BeginActionMotion(const UGGYGOActionMotionProfile* Profile, float PlayRate);
	bool HasActiveActionMotion() const;
	void EndActionMotion(int32 Handle);
	void CleanupFinishedActionMotion();

	UGGYGOCharacterMovementComponent& Movement;
	int32 ActiveActionMotionHandle = INDEX_NONE;
	int32 NextActionMotionHandle = 1;
	uint16 ActionMotionSourceID = 0;
	TSharedPtr<const FGGYGOActionMotionResource> ActiveProfileActionResource;
	TSharedPtr<const FGGYGOActionMotionResource> ActiveMontageActionResource;
	/** At most the original draining/completed resource; retained until its GA retires or a successor starts. */
	TSharedPtr<const FGGYGOActionMotionResource> CompletedMontageActionResource;
	/** Derived only from this resource's consumed final native contribution; no second action clock. */
	bool bCompletedMontageActionNativeContributionConsumed = false;
	float ActionSkippedMovementTickTime = 0.0f;
	FGGYGOActionMotionFailureDelegate ActionMotionFailureCallback;
	TSharedPtr<const FGGYGOActionMotionResource> ActionMotionFailureResource;
	FGGYGOActionMotionCompletionDelegate ActionMotionCompletionCallback;
	TSharedPtr<const FGGYGOActionMotionResource> ActionMotionCompletionResource;
	bool bMontageActionCompletionObserverInstalled = false;
	TSharedPtr<const FGGYGOActionMotionResource> MontageActionNaturalBlendOutResource;
	int32 ActionFailurePreparationDepth = 0;
	/** Exact last terminal token for idempotent release, without retaining a historical resource. */
	int32 LastRetiredMontageActionHandle = INDEX_NONE;
};

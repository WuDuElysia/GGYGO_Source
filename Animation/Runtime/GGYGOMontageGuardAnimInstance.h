// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/Runtime/GGYGOMontagePlayGuard.h"

#include "GGYGOMontageGuardAnimInstance.generated.h"

/**
 * Native montage return barrier. Owns only a game-thread chain of borrowed caller scopes.
 * Engine montage instances and GAS playback state remain owned by their existing executors.
 * The project AnimInstance inheritance and ASC callers are intentionally not connected in A2.
 */
UCLASS(Abstract, BlueprintType, Blueprintable)
class GGYGO_API UGGYGOMontageGuardAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	virtual void NativeInitializeAnimation() override;
	virtual void NativeUninitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;

	/**
	 * Read-only game-thread query of the original instance, lifecycle, issued call ID and owner.
	 * Does not refresh lifecycle state or require a montage instance to exist/be active.
	 * No ASC/GA ownership or Accepted proof; unobserved owner A -> B -> A is not detectable.
	 */
	bool IsMontagePlayGuardIdentityCurrent(const FGGYGOMontagePlayGuardIdentity& Identity) const;

	/**
	 * Instantaneous game-thread view of the innermost live scope for the original coordinator.
	 * Unavailable queries reset outputs to NotEntered/false; no ancestor lookup or state refresh.
	 * Returned does not mean ASC Super finished; completed does not mean source publication.
	 * Requery after external calls. Neither success nor unavailable establishes Task/GA ownership.
	 */
	bool TryGetCurrentMontagePlayGuardStage(const UObject* CallerIdentity,
		EGGYGOMontagePlayGuardNativeStage& OutNativeStage, bool& bOutCompleted) const;

protected:
	virtual float Montage_PlayInternal(UAnimMontage* MontageToPlay,
		const FMontageBlendSettings& BlendInSettings, float InPlayRate = 1.f,
		EMontagePlayReturnType ReturnValueType = EMontagePlayReturnType::MontageLength,
		float InTimeToStartMontageAt = 0.f, bool bStopAllMontages = true) override;

private:
	friend class FGGYGOMontagePlayGuardScope;

	void RegisterScope(FGGYGOMontagePlayGuardScope& Scope);
	void UnregisterScope(FGGYGOMontagePlayGuardScope& Scope);
	bool CanExecuteScope(FGGYGOMontagePlayGuardScope& Scope);
	void CompleteScope(FGGYGOMontagePlayGuardScope& Scope);
	bool ValidateScopeContext(FGGYGOMontagePlayGuardScope& Scope);
	bool ValidateEnclosingScope(FGGYGOMontagePlayGuardScope& Scope);
	bool ObserveCreatedInstance(FGGYGOMontagePlayGuardScope& Scope);
	bool IsCreatedInstanceActive(const FGGYGOMontagePlayGuardScope& Scope);
	void PublishAcceptedCall(const FGGYGOMontagePlayGuardScope& Scope);
	void InvalidateGuardLifecycle();
	void RefreshLifecycleOwner();

	/** Nonowning; invalidation never clears it. Each old scope unregisters itself by identity. */
	TArray<FGGYGOMontagePlayGuardScope*> ScopeChain;
	/** Derived lifecycle observation only, not Pawn/Avatar ownership authority. */
	TWeakObjectPtr<UObject> LifecycleOwner;
	uint64 LifecycleGeneration = 0;
	uint64 LastCallId = 0;
	bool bHadLifecycleOwner = false;
	bool bLifecycleReady = false;
	bool bIdentityExhausted = false;
	bool bUnscopedNativeExecuting = false;
};

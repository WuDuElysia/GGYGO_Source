#pragma once

#include "AbilitySystem/Tests/GGYGOAbilityAdmissionTestTypes.h"

#include "GGYGOInputActivationOriginTestTypes.generated.h"

/** Observations belong only to this fixture instance, never to production input state. */
struct FGGYGOInputActivationOriginCostObservation
{
	int32 OuterCostChecks = 0;
	int32 InnerCostChecks = 0;
	int32 RawTryCalls = 0;
	int32 InnerFailureFeedback = 0;
	int32 OuterFailureFeedback = 0;
	FGameplayAbilitySpecHandle OuterHandle;
	FGameplayAbilitySpecHandle InnerHandle;
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> OuterASC;
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> InnerASC;
	bool bOuterPrimaryMatches = false;
	bool bInnerPrimaryMatches = false;
	bool bRawTryReturned = false;
	bool bRawTrySucceeded = false;
	FString SetupFailure;
};

/** Reuses real Queued admission; only an armed CheckCost injects one raw GAS call. */
UCLASS(Transient)
class UGGYGOInputActivationOriginTestAbility : public UGGYGOAbilityAdmissionQueuedTestAbility
{
	GENERATED_BODY()

public:
	UGGYGOInputActivationOriginTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	void ArmSameSpecRawTryForTest();
	void DisarmForTest();
	bool IsRawTryInFlightForTest() const { return bRawTryInFlight; }
	const FGGYGOInputActivationOriginCostObservation& GetCostObservationForTest() const { return Observation; }

protected:
	virtual bool CheckCost(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayTagContainer* OptionalRelevantTags) const override;
	virtual void NativeOnAbilityFailedToActivate(const FGameplayTagContainer& FailedReason) const override;

private:
	mutable FGGYGOInputActivationOriginCostObservation Observation;
	mutable bool bObserveChecks = false;
	mutable bool bRawTryArmed = false;
	mutable bool bRawTryInFlight = false;
};

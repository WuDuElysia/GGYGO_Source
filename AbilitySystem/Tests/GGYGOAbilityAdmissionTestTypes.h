#pragma once

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "AbilitySystem/Groups/GGYGOAbilityGroupConfig.h"
#include "Abilities/GameplayAbility.h"
#include "Abilities/GameplayAbilityTargetTypes.h"

#include "GGYGOAbilityAdmissionTestTypes.generated.h"

enum class EGGYGOAbilityAdmissionLifecycleObservation : uint8
{
	ActivateBeforeSuper,
	ActivateAfterSuper,
	EndBeforeSuper
};

struct FGGYGOAbilityAdmissionLifecycleSample
{
	UGameplayAbility* Ability = nullptr;
	EGGYGOAbilityAdmissionLifecycleObservation Observation = EGGYGOAbilityAdmissionLifecycleObservation::ActivateBeforeSuper;
	int32 SpecActiveCount = INDEX_NONE;
	bool bIsActive = false;
	bool bReplicateEndAbility = false;
	bool bWasCancelled = false;
};

DECLARE_DELEGATE_OneParam(FGGYGOAbilityAdmissionLifecycleObserver, const FGGYGOAbilityAdmissionLifecycleSample&);

/** Test-only ability whose activation and correction hooks expose real GAS lifecycle results. */
UCLASS(Transient)
class UGGYGOAbilityAdmissionTestAbility : public UGGYGOGameplayAbility
{
	GENERATED_BODY()

public:
	UGGYGOAbilityAdmissionTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	int32 GetBusinessActivationCountForTest() const { return BusinessActivationCount; }
	void FinishForTest();
	bool ActivateWithInvalidPredictionKeyForTest(FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo);

protected:
	virtual void ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;

	int32 BusinessActivationCount = 0;
};

UCLASS(Transient)
class UGGYGOAbilityAdmissionHigherTestAbility : public UGGYGOAbilityAdmissionTestAbility
{
	GENERATED_BODY()

public:
	UGGYGOAbilityAdmissionHigherTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

UCLASS(Transient)
class UGGYGOAbilityAdmissionHighestTestAbility : public UGGYGOAbilityAdmissionTestAbility
{
	GENERATED_BODY()

public:
	UGGYGOAbilityAdmissionHighestTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

/** Per-execution fixture used to verify pending admission alongside an active instance of the same Spec. */
UCLASS(Transient)
class UGGYGOAbilityAdmissionPerExecutionTestAbility : public UGGYGOAbilityAdmissionTestAbility
{
	GENERATED_BODY()

public:
	UGGYGOAbilityAdmissionPerExecutionTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	static FGGYGOAbilityAdmissionLifecycleObserver LifecycleObserver;

protected:
#if WITH_DEV_AUTOMATION_TESTS
	virtual void ObserveAbilityActivationEntryForTest(FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;
	virtual void ObserveAbilityActivationReturnForTest(FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;
	virtual void ObserveAbilityEndEntryForTest(FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility, bool bWasCancelled) override;
#endif

private:
	void ObserveLifecycle(EGGYGOAbilityAdmissionLifecycleObservation Observation,
		const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilitySpecHandle Handle,
		bool bReplicateEndAbility = false, bool bWasCancelled = false);
};

UCLASS(Transient)
class UGGYGOAbilityAdmissionUncancelableTestAbility : public UGGYGOAbilityAdmissionTestAbility
{
	GENERATED_BODY()

public:
	UGGYGOAbilityAdmissionUncancelableTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

protected:
	virtual void InitializeAbilityActivation(const FGGYGOAbilityActivationHandle& Original) override;
};

UCLASS(Transient)
class UGGYGOAbilityAdmissionQueuedTestAbility : public UGGYGOAbilityAdmissionTestAbility
{
	GENERATED_BODY()

public:
	UGGYGOAbilityAdmissionQueuedTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

UCLASS(Transient)
class UGGYGOAbilityAdmissionCoexistTestAbility : public UGGYGOAbilityAdmissionTestAbility
{
	GENERATED_BODY()

public:
	UGGYGOAbilityAdmissionCoexistTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

UCLASS(Transient)
class UGGYGOAbilityAdmissionCorrectionTestAbility : public UGGYGOAbilityAdmissionTestAbility
{
	GENERATED_BODY()

public:
	UGGYGOAbilityAdmissionCorrectionTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	int32 GetCorrectionCountForTest() const { return CorrectionCount; }
	int32 GetCorrectionTargetCountForTest() const { return CorrectionTargetCount; }
	bool DidReceiveLocationCorrectionForTest() const { return bReceivedLocationCorrection; }

	virtual void ReceiveAbilityCorrection(const FGameplayAbilityTargetDataHandle& Correction) override;

private:
	int32 CorrectionCount = 0;
	int32 CorrectionTargetCount = 0;
	bool bReceivedLocationCorrection = false;
};

/** A valid GAS ability outside the project base, used to verify safe type filtering. */
UCLASS(Transient)
class UGGYGOAbilityAdmissionForeignTestAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UGGYGOAbilityAdmissionForeignTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	void FinishForTest();
};

UCLASS(Transient)
class UGGYGOAbilityAdmissionTestGroupConfig : public UGGYGOAbilityGroupConfig
{
	GENERATED_BODY()

public:
	void SetRuleForTest(FGameplayTag GroupTag, EGGYGOAbilityGroupRule Rule, bool bNewcomerWinsOnTie = true);
};

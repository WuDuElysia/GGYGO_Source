/** @file GGYGOMontageTaskTestTypes.h @brief Montage task lifecycle test ability. */
#pragma once

#include "Abilities/GameplayAbility.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/Runtime/GGYGOMontageGuardAnimInstance.h"

class UAnimMontage;
class UFunction;

#include "GGYGOMontageTaskTestTypes.generated.h"

/** Concrete, otherwise inert ability used by montage task automation tests. */
UCLASS(Transient)
class UGGYGOMontageTaskTestAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UGGYGOMontageTaskTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	void SetK2ActivateActionForTest(TFunction<void()> Action) { K2ActivateAction = MoveTemp(Action); }
	bool EndAndReactivateForTest(TFunction<void()> NewActivationAction);
	void FinishForTest();
	virtual void ProcessEvent(UFunction* Function, void* Parms) override;

protected:
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;

private:
	static FName GetK2ActivateAbilityFunctionNameForTest();

	TFunction<void()> K2ActivateAction;
};

/** Concrete UObject owner for scale-lease token tests. */
UCLASS(Transient)
class UGGYGOMontageScaleLeaseTestOwner : public UObject
{
	GENERATED_BODY()
};

/** Hook invoked after the engine has created a real montage instance. */
UCLASS(Transient)
class UGGYGOMontageTaskTestAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	void SetAfterSuperMontagePlayActionForTest(TFunction<void()> Action)
	{
		AfterSuperMontagePlayAction = MoveTemp(Action);
	}
	int32 GetSuccessfulSuperMontagePlayCountForTest() const { return SuccessfulSuperMontagePlayCount; }

protected:
	virtual float Montage_PlayInternal(UAnimMontage* MontageToPlay, const FMontageBlendSettings& BlendInSettings,
		float InPlayRate = 1.0f, EMontagePlayReturnType ReturnValueType = EMontagePlayReturnType::MontageLength,
		float InTimeToStartMontageAt = 0.0f, bool bStopAllMontages = true) override;

private:
	TFunction<void()> AfterSuperMontagePlayAction;
	int32 SuccessfulSuperMontagePlayCount = 0;
};

/** Read-only GAS montage observations; no production state mutation or play override. */
UCLASS(Transient)
class UGGYGOMontageStartedDiagnosticASC : public UGGYGOAbilitySystemComponent
{
	GENERATED_BODY()

public:
	FGameplayAbilityLocalAnimMontage ReadLocalMontageForTest() const { return LocalAnimMontageInfo; }
	FGameplayAbilityRepAnimMontage ReadRepMontageForTest() const { return GetRepAnimMontageInfo(); }
};

/** One-shot observer of the real engine Started broadcast, unbound before running its action. */
UCLASS(Transient)
class UGGYGOMontageStartedDiagnosticObserver : public UObject
{
	GENERATED_BODY()

public:
	void ArmForTest(UAnimInstance* AnimInstance, UAnimMontage* Montage, TFunction<void()> Action);
	void DisarmForTest();

private:
	UFUNCTION()
	void HandleMontageStarted(UAnimMontage* Montage);

	TWeakObjectPtr<UAnimInstance> ObservedAnimInstance;
	TWeakObjectPtr<UAnimMontage> ObservedMontage;
	TFunction<void()> StartedAction;
};

/** Concrete guard instance: uses the production play and native lifecycle without overrides. */
UCLASS(Transient)
class UGGYGOGuardedMontageStartedTestAnimInstance : public UGGYGOMontageGuardAnimInstance
{
	GENERATED_BODY()
};

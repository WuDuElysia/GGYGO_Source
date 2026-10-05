#pragma once

#include "AbilitySystem/Abilities/GGYGOPlayerComboAbility.h"
#include "Animation/Runtime/GGYGOMontageGuardAnimInstance.h"
#include "GameplayEffect.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"

#include "GGYGOPlayerComboLifecycleTestTypes.generated.h"

class UAnimMontage;
class UFunction;

/** The real CMC entry is part of the Combo prerequisite; no motion gate is bypassed for tests. */
UCLASS(Transient)
class AGGYGOPlayerComboLifecycleTestCharacter : public ACharacter
{
	GENERATED_BODY()
public:
	AGGYGOPlayerComboLifecycleTestCharacter(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

/** Native input is explicit for this GAS lifecycle fixture; it creates no physical Source request. */
UCLASS(Transient)
class AGGYGOPlayerComboLifecycleTestController : public APlayerController
{
	GENERATED_BODY()
public:
	AGGYGOPlayerComboLifecycleTestController(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

/** Native fixture that routes the real GAS Blueprint activation dispatch through ProcessEvent. */
UCLASS(Transient)
class UGGYGOPlayerComboLifecycleTestAbility : public UGGYGOPlayerComboAbility
{
	GENERATED_BODY()

public:
	UGGYGOPlayerComboLifecycleTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	void ConfigureStepsForTest(UAnimMontage* Montage, FName TraceStartBone, FName TraceEndBone,
		float FirstPlayRate = 1.0f);
	static FName GetK2ActivateAbilityFunctionName();
	void SetFirstPlayRateForTest(float PlayRate);
	void SetK2ActivateActionForTest(TFunction<void()> Action) { K2ActivateAction = MoveTemp(Action); }
	bool EndAndReactivateForTest();
	bool EndAndReactivateAfterDeferredEndForTest(bool& bDeferredEndWasCoalesced);
	void FinishForTest();
	void UsePredictingActivationModeForTest();
	int32 GetActiveTaskCountForTest() const { return ActiveTasks.Num(); }
	void ConfigureExclusiveSelfPolicyForTest() { SelfPolicy = EGGYGOAbilitySelfPolicy::Exclusive; }
	void SetCanBeCanceledForTest(bool bCanBeCanceled) { SetCanBeCanceled(bCanBeCanceled); }
	void InvalidateNextRequiredSpecForTest() { bInvalidateNextRequiredSpec = true; }
	int32 GetSpecExtensionCountForTest() const { return SpecExtensionCount; }
	int32 GetInvalidatedSpecCountForTest() const { return InvalidatedSpecCount; }

	virtual void ProcessEvent(UFunction* Function, void* Parms) override;

protected:
	virtual void ApplyAbilityTagsToGameplayEffectSpec(FGameplayEffectSpec& Spec,
		FGameplayAbilitySpec* AbilitySpec) const override;

private:
	TFunction<void()> K2ActivateAction;
	mutable bool bInvalidateNextRequiredSpec = false;
	mutable int32 SpecExtensionCount = 0;
	mutable int32 InvalidatedSpecCount = 0;
};

/** A real invalid required-class selection, assigned only after successful fixture activation. */
UCLASS(Abstract, Transient)
class UGGYGOPlayerComboLifecycleInvalidDamageEffect : public UGameplayEffect
{
	GENERATED_BODY()
};

/** Executes a test callback only after Unreal's real Montage_PlayInternal has run. */
UCLASS(Transient)
class UGGYGOPlayerComboLifecycleTestAnimInstance : public UGGYGOMontageGuardAnimInstance
{
	GENERATED_BODY()

public:
	void SetAfterSuperMontagePlayActionForTest(TFunction<void()> Action) { AfterSuperMontagePlayAction = MoveTemp(Action); }
	int32 GetSuccessfulSuperMontagePlayCountForTest() const { return SuccessfulSuperMontagePlayCount; }

protected:
	virtual float Montage_PlayInternal(UAnimMontage* MontageToPlay, const FMontageBlendSettings& BlendInSettings,
		float InPlayRate = 1.f, EMontagePlayReturnType ReturnValueType = EMontagePlayReturnType::MontageLength,
		float InTimeToStartMontageAt = 0.f, bool bStopAllMontages = true) override;

private:
	TFunction<void()> AfterSuperMontagePlayAction;
	int32 SuccessfulSuperMontagePlayCount = 0;
};

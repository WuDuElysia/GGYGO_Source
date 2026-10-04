#pragma once

#include "AbilitySystem/Abilities/GGYGOPlayerComboAbility.h"
#include "Animation/Runtime/GGYGOMontageGuardAnimInstance.h"

#include "GGYGOPlayerComboLifecycleTestTypes.generated.h"

class UAnimMontage;
class UFunction;

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

	virtual void ProcessEvent(UFunction* Function, void* Parms) override;

private:
	TFunction<void()> K2ActivateAction;
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

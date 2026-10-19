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
	void ConfigureDamageMultiplierForTest(float Multiplier);
	void SetK2ActivateActionForTest(TFunction<void()> Action) { K2ActivateAction = MoveTemp(Action); }
	/** 在真实原 cleanup 入口执行一次夹具动作；不改公共 GA／CMC 生命周期入口。 */
	void SetBeforeCleanupActionForTest(TFunction<void()> Action) { BeforeCleanupAction = MoveTemp(Action); }
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
	virtual void CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context) override;
	virtual void ApplyAbilityTagsToGameplayEffectSpec(FGameplayEffectSpec& Spec,
		FGameplayAbilitySpec* AbilitySpec) const override;

private:
	TFunction<void()> K2ActivateAction;
	TFunction<void()> BeforeCleanupAction;
	mutable bool bInvalidateNextRequiredSpec = false;
	mutable int32 SpecExtensionCount = 0;
	mutable int32 InvalidatedSpecCount = 0;
};

/** 使用真实DamageExecution及显式倍率模式的瞬态测试载体。 */
UCLASS(Transient)
class UGGYGOPlayerComboLifecycleMultiplierEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGGYGOPlayerComboLifecycleMultiplierEffect();
};

/** A real invalid required-class selection, assigned only after successful fixture activation. */
UCLASS(Abstract, Transient)
class UGGYGOPlayerComboLifecycleInvalidDamageEffect : public UGameplayEffect
{
	GENERATED_BODY()
};

/** 保留正常播放与拒播叶的 Guard 正返回计数；重入创建事实另由真实 Started 观察者提供。 */
UCLASS(Transient)
class UGGYGOPlayerComboLifecycleTestAnimInstance : public UGGYGOMontageGuardAnimInstance
{
	GENERATED_BODY()

public:
	int32 GetSuccessfulGuardReturnCountForTest() const { return SuccessfulGuardReturnCount; }

protected:
	virtual float Montage_PlayInternal(UAnimMontage* MontageToPlay, const FMontageBlendSettings& BlendInSettings,
		float InPlayRate = 1.f, EMontagePlayReturnType ReturnValueType = EMontagePlayReturnType::MontageLength,
		float InTimeToStartMontageAt = 0.f, bool bStopAllMontages = true) override;

private:
	int32 SuccessfulGuardReturnCount = 0;
};

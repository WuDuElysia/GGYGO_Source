#pragma once

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "AbilitySystem/Executions/GGYGODamageExecution.h"
#include "AbilitySystem/GGYGOAbilitySourceInterface.h"
#include "GameplayEffect.h"

#include "GGYGOHitSemanticsTestTypes.generated.h"

/** Owns the narrow test-only bridge to the protected shared hit-payload builder. */
UCLASS(Transient)
class UGGYGOHitSemanticsTestAbility : public UGGYGOGameplayAbility
{
	GENERATED_BODY()

public:
	bool BuildHitEffectPayloadForTest(UAbilitySystemComponent* TargetAbilitySystemComponent,
		TSubclassOf<UGameplayEffect> DamageEffectClass, float EffectLevel,
		const FHitResult& HitResult, const FVector& Origin,
		FGGYGOHitEffectPayload& OutPayload) const
	{
		return BuildHitEffectPayload(TargetAbilitySystemComponent, DamageEffectClass, EffectLevel,
			HitResult, Origin, OutPayload);
	}

	void FinishForTest()
	{
		if (IsActive())
		{
			EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, false, false);
		}
	}
};

/** Observes the production execution's source-interface inputs; does not execute damage. */
UCLASS(Transient)
class UGGYGOHitDistanceTestSource : public UObject, public IGGYGOAbilitySourceInterface
{
	GENERATED_BODY()

public:
	virtual float GetDistanceAttenuation(float Distance, const FGameplayTagContainer* SourceTags = nullptr,
		const FGameplayTagContainer* TargetTags = nullptr) const override
	{
		ObservedDistances.Add(Distance);
		return 1.0f / (1.0f + Distance);
	}

	virtual float GetPhysicalMaterialAttenuation(const UPhysicalMaterial* PhysicalMaterial,
		const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr) const override
	{
		++PhysicalMaterialCalls;
		return 1.0f;
	}

	mutable TArray<float> ObservedDistances;
	mutable int32 PhysicalMaterialCalls = 0;
};

/** A test-only definition selecting the real production calculation through native GAS. */
UCLASS(Transient)
class UGGYGOHitDistanceTestEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UGGYGOHitDistanceTestEffect()
	{
		DurationPolicy = EGameplayEffectDurationType::Instant;
		FGameplayEffectExecutionDefinition& Execution = Executions.AddDefaulted_GetRef();
		Execution.CalculationClass = UGGYGODamageExecution::StaticClass();
	}
};

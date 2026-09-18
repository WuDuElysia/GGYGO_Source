/** @file GGYGOBossMeleeAbility.h @brief Montage Event 驱动的 Boss 近战竖切 */
#pragma once

#include "AbilitySystem/Abilities/GGYGOCombatActionAbility.h"

#include "GGYGOBossMeleeAbility.generated.h"

class UAnimMontage;
class UGameplayEffect;
class UGGYGOAbilityTask_PlayMontageAndWaitForEvent;
class UGGYGOMeleeTraceComponent;
struct FHitResult;

/**
 * 一条完整的近战执行链：Montage Event 开/关 Trace，命中后应用 Damage GE 并发 Cue。
 * AI 只激活本能力，不直接操作 Montage、Trace 或伤害。
 */
UCLASS(Blueprintable)
class GGYGO_API UGGYGOBossMeleeAbility : public UGGYGOCombatActionAbility
{
	GENERATED_BODY()

public:
	UGGYGOBossMeleeAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility, bool bWasCancelled) override;

	UFUNCTION()
	void HandleMontageCompleted(FGameplayTag EventTag, FGameplayEventData EventData);

	UFUNCTION()
	void HandleMontageInterrupted(FGameplayTag EventTag, FGameplayEventData EventData);

	UFUNCTION()
	void HandleMontageEvent(FGameplayTag EventTag, FGameplayEventData EventData);

	UFUNCTION()
	void HandleMeleeHit(AActor* HitActor, const FHitResult& HitResult);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee")
	TObjectPtr<UAnimMontage> AttackMontage;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee")
	TSubclassOf<UGameplayEffect> DamageEffect;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee", meta = (ClampMin = "0.0"))
	float Damage = 20.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee", meta = (ClampMin = "0.0"))
	float PoiseDamage = 10.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee")
	FName TraceStartSocket = TEXT("hand_r");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee")
	FName TraceEndSocket = TEXT("index_03_r");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee", meta = (ClampMin = "1.0"))
	float TraceRadius = 24.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee", meta = (Categories = "GameplayCue.Hit"))
	FGameplayTag HitCueTag;

	UPROPERTY(Transient)
	TObjectPtr<UGGYGOMeleeTraceComponent> ActiveTraceComponent;

	UPROPERTY(Transient)
	TObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> MontageTask;
};

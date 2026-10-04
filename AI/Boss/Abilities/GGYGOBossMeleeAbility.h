/** @file GGYGOBossMeleeAbility.h @brief Montage Event 驱动的 Boss 近战竖切 */
#pragma once

#include "AbilitySystem/Abilities/GGYGOCombatActionAbility.h"
#include "Components/SkinnedMeshComponent.h"

#include "GGYGOBossMeleeAbility.generated.h"

class UAnimMontage;
class UGameplayEffect;
class UGGYGOAbilityTask_PlayMontageAndWaitForEvent;
class UGGYGOMeleeTraceComponent;
class USkeletalMeshComponent;
class UGGYGOActionMotionProfile;
class UGGYGOCharacterMovementComponent;
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
	bool ValidateMeleeConfiguration(FString& OutError) const;
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif

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
	void HandleMontageBlendOut(FGameplayTag EventTag, FGameplayEventData EventData);
	void HandleMontageTimeout();

	UFUNCTION()
	void HandleMontageEvent(FGameplayTag EventTag, FGameplayEventData EventData);

	UFUNCTION()
	void HandleMeleeHit(AActor* HitActor, const FHitResult& HitResult);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee")
	TObjectPtr<UAnimMontage> AttackMontage;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee", meta = (ClampMin = "0.01"))
	float MontagePlayRate = 1.0f;
	/** 可空；GA 持有动作期句柄，CMC 采样位移。仅支持从零开始、固定速率且不跳 Section 的线性 Montage。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee")
	TObjectPtr<const UGGYGOActionMotionProfile> ActionMotionProfile;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee")
	TSubclassOf<UGameplayEffect> DamageEffect;
	/** 仅DamageEffect为空时选择共享预载GE；默认false保留旧空值配置失败语义。
	 *  非空DamageEffect始终优先；启用但共享不可用仍校验失败，命中不会退化为无伤害成功。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee")
	bool bUseSharedDamageEffectWhenUnset = false;

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

	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> ActiveMesh;
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOCharacterMovementComponent> ActiveMotionMovement;
	int32 ActionMotionHandle = INDEX_NONE;
	FTimerHandle MontageTimeoutHandle;
	EVisibilityBasedAnimTickOption SavedMeshTick = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	bool bSavedUpdateRateOptimizations = false;
	bool bChangedMeshTick = false;
	bool bCleaningUp = false;
	bool bBlendingOut = false;
};

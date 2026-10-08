/** @file GGYGOBossMeleeAbility.h @brief Montage Event 驱动的 Boss 近战竖切 */
#pragma once

#include "AbilitySystem/Abilities/GGYGOCombatActionAbility.h"

#include "GGYGOBossMeleeAbility.generated.h"

class UAnimMontage;
class UGameplayEffect;
class UGGYGOAbilityTask_PlayMontageAndWaitForEvent;
class UGGYGOMeleeTraceComponent;
class USkeletalMeshComponent;
class UGGYGOActionMotionProfile;
class UGGYGOCharacterMovementComponent;
struct FHitResult;
struct FGGYGOMeleeTraceWindowHandle;

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
	virtual void InitializeAbilityActivation(const FGGYGOAbilityActivationHandle& Original) override;
	virtual void ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context) override;
	/** Prepare this original batch's actual Mesh restoration resource; no lifecycle state is issued. */
	bool PrepareOriginalMeleeMesh(const FGGYGOAbilityActivationHandle& Original, USkeletalMeshComponent* Mesh);
	/** Start and observe this batch's Profile motion; CMC owns execution and observer retirement. */
	bool BeginOriginalMeleeMotion(const FGGYGOAbilityActivationHandle& Original,
		UGGYGOCharacterMovementComponent* Movement, const UGGYGOActionMotionProfile* Profile, float PlayRate);
	bool HasOriginalMeleeMeshResource() const;
	FGGYGOAbilityActivationHandle GetOriginalMeleeResourceActivation() const;

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

private:
	struct FOriginalMeleeResources;
	// Resources and issued provenance only; GA/ASC remain the lifecycle authorities.
	TSharedPtr<FOriginalMeleeResources> OriginalResources;
	enum class EMontageCallback : uint8 { Completed, Interrupted, BlendOut, Event };

	bool OwnsOriginalResources(const TSharedPtr<FOriginalMeleeResources>& Resources,
		const FGGYGOAbilityActivationHandle& Original) const;
	bool IsOriginalResourcesCurrent(const TSharedPtr<FOriginalMeleeResources>& Resources,
		const FGGYGOAbilityActivationHandle& Original) const;
	bool AreOriginalReceiversCurrent(const TSharedPtr<FOriginalMeleeResources>& Resources) const;
	bool IsOriginalTaskCurrent(const TSharedPtr<FOriginalMeleeResources>& Resources,
		const FGGYGOAbilityActivationHandle& Original,
		const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent>& OriginalTask) const;
	void RequestOriginalTermination(const FGGYGOAbilityActivationHandle& Original,
		bool bCancel, bool bWasCancelled);
	void FailOriginalAction(const TSharedPtr<FOriginalMeleeResources>& Resources,
		const FGGYGOAbilityActivationHandle& Original, const TCHAR* Stage, const FString& Reason);
	void CloseOriginalTraceWindow(const TSharedPtr<FOriginalMeleeResources>& Resources);
	void HandleOriginalMontageCallback(const TSharedPtr<FOriginalMeleeResources>& Resources,
		const FGGYGOAbilityActivationHandle& Original,
		const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent>& OriginalTask,
		EMontageCallback Kind, FGameplayTag EventTag, const FGameplayEventData& EventData);
	void HandleOriginalMontageTimeout(const TSharedPtr<FOriginalMeleeResources>& Resources,
		const FGGYGOAbilityActivationHandle& Original,
		const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent>& OriginalTask);
	void HandleOriginalMeleeHit(const TSharedPtr<FOriginalMeleeResources>& Resources,
		const FGGYGOAbilityActivationHandle& Original,
		const TWeakObjectPtr<UGGYGOMeleeTraceComponent>& OriginalTrace,
		const FGGYGOMeleeTraceWindowHandle& Window, AActor* HitActor, const FHitResult& HitResult);
};

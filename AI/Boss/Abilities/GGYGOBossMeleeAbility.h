/** @file GGYGOBossMeleeAbility.h @brief Montage Event 驱动的 Boss 近战竖切 */
#pragma once

#include "AbilitySystem/Abilities/GGYGOCombatActionAbility.h"
#include "AbilitySystem/Abilities/GGYGOMontageWindowFXTypes.h"

#include "GGYGOBossMeleeAbility.generated.h"

class UAnimMontage;
class UGameplayEffect;
class UGGYGOAbilityTask_PlayMontageAndWaitForEvent;
class UGGYGOMeleeTraceComponent;
class USkeletalMeshComponent;
class UGGYGOActionMotionProfile;
class UGGYGOCharacterMovementComponent;
struct FGGYGOMontageFXLocalStartReadyFact;
struct FHitResult;
struct FGGYGOMeleeTraceWindowHandle;
struct FGGYGOActionMotionSourceBinding;
struct FGGYGOMontageTaskFailureFact;
struct FGGYGOMontageWindowFact;
struct FGGYGOMontageActionResources;

/** Montage 作者提供的一对独立窗口事件；时点只来自原资产。 */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOBossWindowTags
{
	GENERATED_BODY()
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	FGameplayTag Begin;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	FGameplayTag End;
};

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
	/** Validate the configured attack source and explicit motion mode without starting resources.
	 * @param OutError Empty on success; diagnostic identifying invalid required configuration on failure.
	 * @return Whether this ability's asset configuration is valid; runtime receivers are checked on activation. */
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
	/** 派生 Boss 动作只消费已由原公共 Task 认证的窗口；返回 true 表示已消费。 */
	virtual bool ConsumeAdditionalWindow(const FGGYGOAbilityActivationHandle& Original,
		const FGGYGOMontageWindowFact& Fact) { return false; }
	virtual void AppendMontageEventTags(FGameplayTagContainer& Tags) const {}
	virtual bool UsesMeleeTrace() const { return true; }
	virtual bool RequiresDamage() const { return true; }
	virtual void GetWindowDamage(FGameplayTag Begin, float& OutDamage, float& OutPoise) const
	{ OutDamage = Damage; OutPoise = PoiseDamage; }
	virtual FGameplayTag GetWindowHitCue(FGameplayTag Begin) const { return HitCueTag; }
	virtual bool ValidateBossAttackConfiguration(FString& OutError) const { return true; }
	/** 原 Montage/CMC 完成后交派生业务编排；默认沿旧能力正常结束。 */
	virtual void OnOriginalMontageActionCompleted(const FGGYGOAbilityActivationHandle& Original);
	bool IsOriginalMontageActionComplete(const FGGYGOAbilityActivationHandle& Original) const;
	bool HasOriginalActionReceivers(const FGGYGOAbilityActivationHandle& Original) const;
	bool IsOriginalActionWindow(const FGGYGOAbilityActivationHandle& Original, const FGGYGOMontageWindowFact& Fact) const;
	/** 仅供已认证原窗口的底层执行；不选择目标或业务时点。 */
	void ExecuteOriginalMeleeWindow(const FGGYGOAbilityActivationHandle& Original, const FGGYGOMontageWindowFact& Fact);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee")
	TObjectPtr<UAnimMontage> AttackMontage;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee", meta = (ClampMin = "0.01"))
	float MontagePlayRate = 1.0f;
	/** 可空；GA 持有动作期句柄，CMC 采样位移。仅支持从零开始、固定速率且不跳 Section 的线性 Montage。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee")
	TObjectPtr<const UGGYGOActionMotionProfile> ActionMotionProfile;
	/** Explicit original-Montage XYZ mode; incompatible with ActionMotionProfile.
	 * False retains the optional Profile contract, including deliberately absent motion. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee|Motion")
	bool bUseMontageSourceMotion = false;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee|Motion", meta = (EditCondition = "bUseMontageSourceMotion"))
	FName MotionSlot = TEXT("DefaultSlot");
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee|Motion", meta = (EditCondition = "bUseMontageSourceMotion"))
	FName MotionSection = TEXT("Main");
	/** None 保留原单 Section 合同；完整多段动作显式填写最后 Section，由 CMC 执行整条来源。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee|Motion", meta = (EditCondition = "bUseMontageSourceMotion"))
	FName MotionEndSection = NAME_None;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss Melee|Motion", meta = (EditCondition = "bUseMontageSourceMotion", ClampMin = "0.0"))
	float MotionTranslationScale = 1.0f;

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

	/** 原作者窗内的完整表现组合，Task、发布及尾部均归本次 GA 资源。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|FX")
	TArray<FGGYGOComboStepFX> MontageFXWindows;
	/** 空数组保留原通用 HitWindowBegin/End；新动作逐作者窗显式配置。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Hit")
	TArray<FGGYGOBossWindowTags> HitWindowTags;

private:
	struct FOriginalMeleeResources;
	// Resources and issued provenance only; GA/ASC remain the lifecycle authorities.
	TSharedPtr<FOriginalMeleeResources> OriginalResources;
	enum class EMontageCallback : uint8 { Completed, Interrupted, BlendOut };
	bool ResolveMeleeConfiguration(FString& OutError,
		TSharedPtr<const FGGYGOActionMotionSourceBinding, ESPMode::ThreadSafe>& OutSource,
		TArray<FGGYGOMontageWindowDeclaration>* OutFXWindows = nullptr) const;
	bool BeginOriginalMontageMotion(const TSharedPtr<FOriginalMeleeResources>& Resources);
	void TryCompleteOriginalMontageMotion(const TSharedPtr<FOriginalMeleeResources>& Resources);
	void HandleOriginalTaskFailure(const TSharedPtr<FOriginalMeleeResources>& Resources,
		const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent>& OriginalTask,
		const FGGYGOMontageTaskFailureFact& Fact);

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
		EMontageCallback Kind);
	void HandleOriginalWindow(const TSharedPtr<FOriginalMeleeResources>& Resources,
		const FGGYGOAbilityActivationHandle& Original,
		const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent>& OriginalTask,
		const FGGYGOMontageWindowFact& Fact);
	void HandleOriginalFXReady(const TSharedPtr<FOriginalMeleeResources>& Resources,
		const TSharedPtr<FGGYGOMontageActionResources>& FXResource,
		const FGGYGOMontageFXLocalStartReadyFact& Fact);
	UFUNCTION()
	void HandleOriginalFXFailed(UGGYGOAbilityTask_PlayNiagaraEffect* Sender, const FString& Reason);
	void HandleOriginalMontageTimeout(const TSharedPtr<FOriginalMeleeResources>& Resources,
		const FGGYGOAbilityActivationHandle& Original,
		const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent>& OriginalTask);
	void HandleOriginalMeleeHit(const TSharedPtr<FOriginalMeleeResources>& Resources,
		const FGGYGOAbilityActivationHandle& Original,
		const TWeakObjectPtr<UGGYGOMeleeTraceComponent>& OriginalTrace,
		const FGGYGOMeleeTraceWindowHandle& Window, AActor* HitActor, const FHitResult& HitResult);
};

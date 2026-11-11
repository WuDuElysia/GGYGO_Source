/** @file GGYGOBossAttackAbility.h @brief 配置驱动的 Boss 近战、范围与弹体动作 */
#pragma once
#include "AI/Boss/Abilities/GGYGOBossMeleeAbility.h"
#include "GGYGOBossAttackAbility.generated.h"

class UGGYGOHealthComponent;
class USoundBase;
class UNiagaraSystem;
class APawn;
class AController;
struct FGGYGOCombatOverlapFact;
struct FGGYGOCombatSweepFact;

/** BP 只传递 GAS 已发行的原身份；没有可编辑 serial、来源或当前激活查询。 */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOBossActionToken
{
	GENERATED_BODY()
	FGGYGOAbilityActivationHandle Original;
};
template<> struct TStructOpsTypeTraits<FGGYGOBossActionToken> : TStructOpsTypeTraitsBase2<FGGYGOBossActionToken>
{ enum { WithCopy = true }; };

UENUM(BlueprintType)
enum class EGGYGOBossHitMode : uint8 { Melee, AreaSphere, AreaBox, Projectile, None };

/** 可编辑 Montage 组合中，一个具名作者窗选择的底层查询及结算参数。 */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOBossWindowExecution
{
	GENERATED_BODY()
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	EGGYGOBossHitMode Mode = EGGYGOBossHitMode::None;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	float Damage = 0.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	float PoiseDamage = 0.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	FGameplayTag HitCueTag;
};

/** 持续声的有限作者窗；声音自身是否循环由真实 USoundBase 决定。 */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOBossAudioWindow
{
	GENERATED_BODY()
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	FGGYGOBossWindowTags Events;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	TObjectPtr<USoundBase> Sound;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	FName Socket;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	FTransform RelativeTransform = FTransform::Identity;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	float Volume = 1.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	float Pitch = 1.0f;
};

UCLASS(Blueprintable)
class GGYGO_API UGGYGOBossAttackAbility : public UGGYGOBossMeleeAbility
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintPure, Category = "GGYGO|Boss|资源")
	ACharacter* GetBossCharacter(FGGYGOBossActionToken Token) const;
	UFUNCTION(BlueprintPure, Category = "GGYGO|Boss|资源")
	AActor* GetBoundBossTarget(FGGYGOBossActionToken Token) const;
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Boss|资源")
	bool BindBossTarget(FGGYGOBossActionToken Token, AActor* Target, FString& OutError);
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Boss|资源")
	bool ExecuteBossWindow(FGGYGOBossActionToken Token, FGGYGOMontageWindowFact Window,
		const FTransform& AreaBasis, const FVector& ProjectileAimPoint, FString& OutError);
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Boss|资源")
	bool AcquireBossFlight(FGGYGOBossActionToken Token, FString& OutError);
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Boss|资源")
	bool ReleaseBossFlight(FGGYGOBossActionToken Token, bool bEnterFalling, FString& OutError);
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Boss|资源")
	bool WaitBossLanding(FGGYGOBossActionToken Token, float TimeoutSeconds, FString& OutError);
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Boss|资源")
	void FinishBossAction(FGGYGOBossActionToken Token, bool bCancelled);
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Boss|资源")
	void FailBossAction(FGGYGOBossActionToken Token, const FString& Reason);
	/** BP 在真实落地后显式请求收尾 Montage；仍持原激活，显式纯姿态播放，不创建CMC位移。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Boss|资源")
	bool PlayBossRecoveryPresentation(FGGYGOBossActionToken Token, UAnimMontage* Montage, float PlayRate,
		const FGGYGOComboStepFX& FX, FString& OutError);
protected:
	/** 具体对象在可编辑 BP 图中选择是否借飞行模式；空实现表示普通地面动作。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "GGYGO|Boss|动作")
	void PrepareBossAction(FGGYGOBossActionToken Token);
	UFUNCTION(BlueprintImplementableEvent, Category = "GGYGO|Boss|动作")
	void MontageActionFinished(FGGYGOBossActionToken Token);
	UFUNCTION(BlueprintImplementableEvent, Category = "GGYGO|Boss|动作")
	void BossLanded(FGGYGOBossActionToken Token, const FHitResult& Hit);
	UFUNCTION(BlueprintImplementableEvent, Category = "GGYGO|Boss|动作")
	void RecoveryMontageFinished(FGGYGOBossActionToken Token);
	UFUNCTION(BlueprintImplementableEvent, Category = "GGYGO|Boss|动作")
	void BossHitWindow(FGGYGOBossActionToken Token, FGGYGOMontageWindowFact Window, USkeletalMeshComponent* Mesh);
	UFUNCTION(BlueprintImplementableEvent, Category = "GGYGO|Boss|动作")
	void BossTargetInvalidated(FGGYGOBossActionToken Token, AActor* Target, const FString& Reason);
	UFUNCTION(BlueprintImplementableEvent, Category = "GGYGO|Boss|动作")
	void BossTargetControlChanged(FGGYGOBossActionToken Token, AController* NewController);
	virtual void OnOriginalMontageActionCompleted(const FGGYGOAbilityActivationHandle& Original) override;
	virtual void InitializeAbilityActivation(const FGGYGOAbilityActivationHandle& Original) override;
	virtual void ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo Info, const FGameplayEventData* Event) override;
	virtual void CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context) override;
	virtual bool ConsumeAdditionalWindow(const FGGYGOAbilityActivationHandle& Original,
		const FGGYGOMontageWindowFact& Fact) override;
	virtual void AppendMontageEventTags(FGameplayTagContainer& Tags) const override;
	virtual bool UsesMeleeTrace() const override;
	virtual bool RequiresDamage() const override;
	virtual void GetWindowDamage(FGameplayTag Begin, float& OutDamage, float& OutPoise) const override;
	virtual FGameplayTag GetWindowHitCue(FGameplayTag Begin) const override;
	virtual bool ValidateBossAttackConfiguration(FString& OutError) const override;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Hit")
	EGGYGOBossHitMode HitMode = EGGYGOBossHitMode::Melee;
	/** 空表表示独立单类动作；非空表必须完整覆盖 HitWindowTags，不隐式填缺失项。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Hit")
	TMap<FGameplayTag, FGGYGOBossWindowExecution> WindowExecution;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Area")
	FVector AreaOffset = FVector::ZeroVector;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Area")
	float AreaRadius = 0.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Area")
	FVector AreaHalfExtents = FVector::ZeroVector;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Projectile")
	FName ProjectileSocket;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Projectile")
	FVector ProjectileOffset = FVector::ZeroVector;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Projectile")
	float ProjectileRadius = 0.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Projectile")
	float ProjectileSpeed = 0.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Projectile")
	float ProjectileLifetime = 0.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Projectile")
	float ProjectileGravity = 0.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Projectile")
	TObjectPtr<UNiagaraSystem> ProjectileSystem;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Projectile")
	FTransform ProjectileVisualTransform = FTransform::Identity;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Hit")
	TEnumAsByte<ECollisionChannel> QueryChannel = ECC_Pawn;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Audio")
	TArray<FGGYGOBossAudioWindow> PersistentAudio;
	/** 共享BP流程的输入参数；Native不以这些字段决定编排。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Blueprint Flow")
	float LandingTimeoutSeconds = 0.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Blueprint Flow")
	TObjectPtr<UAnimMontage> LandingRecoveryMontage;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Blueprint Flow")
	float RecoveryPlayRate = 1.0f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Blueprint Flow")
	FGGYGOComboStepFX RecoveryFX;
private:
	struct FAttackResources;
	TSharedPtr<FAttackResources> AttackResources;
	bool IsAttackCurrent(const TSharedPtr<FAttackResources>& Resources) const;
	bool HasConfiguredHitMode(EGGYGOBossHitMode Mode) const;
	void AbortAttack(const TSharedPtr<FAttackResources>& Resources, const FString& Reason);
	void ApplyAreaFact(const TSharedPtr<FAttackResources>& Resources, const FGGYGOCombatOverlapFact& Fact, float HitDamage, float HitPoise, FGameplayTag Cue);
	void ApplySweepFact(const TSharedPtr<FAttackResources>& Resources, const FGGYGOCombatSweepFact& Fact, float HitDamage, float HitPoise, FGameplayTag Cue);
	void OpenQueryWindow(const TSharedPtr<FAttackResources>& Resources, const FGGYGOMontageWindowFact& Fact,
		EGGYGOBossHitMode Mode, const FTransform& AreaBasis, const FVector& ProjectileAimPoint);
	void CompleteRecovery(const TSharedPtr<FAttackResources>& Resources);
	void ContinueRecoveryFX(const TSharedPtr<FAttackResources>& Resources, const FGGYGOMontageFXLocalStartReadyFact& Fact);
	UFUNCTION()
	void OnRecoveryFXFailed(UGGYGOAbilityTask_PlayNiagaraEffect* Sender, const FString& Reason);
	UFUNCTION()
	void OnOriginalDeath(AActor* Actor);
	UFUNCTION()
	void OnOriginalEndPlay(AActor* Actor, EEndPlayReason::Type Reason);
	UFUNCTION()
	void OnOwnedLanded(const FHitResult& Hit);
	UFUNCTION()
	void OnOriginalTargetControllerChanged(APawn* Pawn, AController* OldController, AController* NewController);
};

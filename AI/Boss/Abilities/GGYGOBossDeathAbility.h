/** @file GGYGOBossDeathAbility.h @brief Health 驱动 ABP 时的死亡资源与保留时长 */
#pragma once
#include "AbilitySystem/Abilities/GGYGODeathAbility.h"
#include "AI/Boss/Abilities/GGYGOBossAttackAbility.h"
#include "TimerManager.h"
#include "GGYGOBossDeathAbility.generated.h"
class UGGYGOHealthComponent;

/** ABP 是姿态唯一作者；能力只负责死亡生命周期，不另播死亡 Montage。 */
UCLASS(Blueprintable)
class GGYGO_API UGGYGOBossDeathAbility : public UGGYGODeathAbility
{
	GENERATED_BODY()
public:
	UGGYGOBossDeathAbility(const FObjectInitializer& Initializer = FObjectInitializer::Get());
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Boss|死亡资源")
	bool CancelOtherBossActions(FGGYGOBossActionToken Token, FString& OutError);
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Boss|死亡资源")
	bool StartBossDeathState(FGGYGOBossActionToken Token, FString& OutError);
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Boss|死亡资源")
	bool WaitBossDeathPresentation(FGGYGOBossActionToken Token, float Seconds, FString& OutError);
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Boss|死亡资源")
	void FinishBossDeath(FGGYGOBossActionToken Token, bool bCancelled);
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Boss|死亡资源")
	void FailBossDeath(FGGYGOBossActionToken Token, const FString& Reason);
protected:
	UFUNCTION(BlueprintImplementableEvent, Category = "GGYGO|Boss|死亡流程")
	void PrepareBossDeath(FGGYGOBossActionToken Token);
	UFUNCTION(BlueprintImplementableEvent, Category = "GGYGO|Boss|死亡流程")
	void BossDeathPresentationFinished(FGGYGOBossActionToken Token);
	virtual void ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo Info, const FGameplayEventData* Event) override;
	virtual void CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context) override;
	/** 来自实际 Die 资产的保留时长，不宣称定时器证明 GPU/动画完成。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Boss|Death")
	float PresentationSeconds = 0.0f;
private:
	struct FDeathResources;
	TSharedPtr<FDeathResources> DeathResources;
	bool IsDeathResourceCurrent(FGGYGOBossActionToken Token) const;
};

/**
 * @file GGYGOBossState.h
 * @brief 可跨形态 Pawn 持续存在的 Boss ASC 宿主
 */
#pragma once

#include "Combatants/GGYGOCombatantState.h"
#include "GameplayTagContainer.h"

#include "GGYGOBossState.generated.h"

class UGGYGOBossDefinition;
class UGGYGOPawnData;
struct FGameplayEffectSpec;

/**
 * Boss 的持久战斗状态；拥有阶段身份，消费 HealthSet 原始生命变化，不包含 BT 或生成逻辑。
 */
UCLASS(BlueprintType, Blueprintable, meta = (ShortTooltip = "Boss 的持久 ASC 与阶段/形态状态"))
class GGYGO_API AGGYGOBossState : public AGGYGOCombatantState
{
	GENERATED_BODY()

public:
	AGGYGOBossState(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** 仅服务器、仅一次。验证初始 Form/Phase 及各阶段 ActionSet 后一次性授予全部已声明能力；
	 *  采纳后的 ActionSet 不再在选招周期内整表复验。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Boss")
	bool InitializeFromDefinition(const UGGYGOBossDefinition* InDefinition);

	UFUNCTION(BlueprintPure, Category = "GGYGO|Boss")
	const UGGYGOBossDefinition* GetBossDefinition() const { return BossDefinition; }

	UFUNCTION(BlueprintPure, Category = "GGYGO|Boss")
	const UGGYGOPawnData* GetInitialPawnData() const;

	UFUNCTION(BlueprintPure, Category = "GGYGO|Boss")
	FGameplayTag GetCurrentFormTag() const { return CurrentFormTag; }

	UFUNCTION(BlueprintPure, Category = "GGYGO|Boss")
	FGameplayTag GetCurrentPhaseTag() const { return CurrentPhaseTag; }

	/** 仅更换已声明的动作池；不会取消或替换 GAS 正在执行的能力。 */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "GGYGO|Boss")
	bool SetCurrentPhase(FGameplayTag NewPhaseTag);

	/** 初始化完成及 Health/MaxHealth 实变后通知当前事实；具体阶段条件由蓝图实现。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "GGYGO|Boss")
	void OnBossHealthChanged(float Health, float MaxHealth);

protected:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	void SetReplicatedStateTag(FGameplayTag& CurrentTag, FGameplayTag NewTag);

	UFUNCTION()
	void OnRep_BossDefinition();

	UPROPERTY(ReplicatedUsing = OnRep_BossDefinition, VisibleInstanceOnly, BlueprintReadOnly, Category = "GGYGO|Boss")
	TObjectPtr<const UGGYGOBossDefinition> BossDefinition;

	UPROPERTY(Replicated, VisibleInstanceOnly, BlueprintReadOnly, Category = "GGYGO|Boss")
	FGameplayTag CurrentFormTag;

	UPROPERTY(Replicated, VisibleInstanceOnly, BlueprintReadOnly, Category = "GGYGO|Boss")
	FGameplayTag CurrentPhaseTag;

private:
	/** 从初始形态 PawnData 将规则配置应用到 ASC；可由初始化与复制回调重复调用。 */
	void ApplyAbilitySystemConfiguration();
	void HandlePhaseHealthChanged(AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec,
		float Magnitude, float OldValue, float NewValue);
	FDelegateHandle PhaseHealthChanged;
	FDelegateHandle PhaseMaxHealthChanged;
};

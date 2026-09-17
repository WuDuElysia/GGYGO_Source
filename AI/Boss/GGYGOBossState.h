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

/**
 * Boss 的持久战斗状态。阶段 B 只负责一次初始化与单形态装配；不包含 BT、仇恨或生成逻辑。
 */
UCLASS(BlueprintType, meta = (ShortTooltip = "Boss 的持久 ASC 与阶段/形态状态"))
class GGYGO_API AGGYGOBossState : public AGGYGOCombatantState
{
	GENERATED_BODY()

public:
	AGGYGOBossState(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** 仅服务器、仅一次。验证初始 Form/Phase 后一次性授予全部已声明能力。 */
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

protected:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	void SetReplicatedStateTag(FGameplayTag& CurrentTag, FGameplayTag NewTag);

	UPROPERTY(Replicated, VisibleInstanceOnly, BlueprintReadOnly, Category = "GGYGO|Boss")
	TObjectPtr<const UGGYGOBossDefinition> BossDefinition;

	UPROPERTY(Replicated, VisibleInstanceOnly, BlueprintReadOnly, Category = "GGYGO|Boss")
	FGameplayTag CurrentFormTag;

	UPROPERTY(Replicated, VisibleInstanceOnly, BlueprintReadOnly, Category = "GGYGO|Boss")
	FGameplayTag CurrentPhaseTag;

};

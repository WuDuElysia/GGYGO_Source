/**
 * @file GGYGOBossDefinition.h
 * @brief Boss 静态定义：阶段 B 只消费一个初始形态与一个初始阶段
 */
#pragma once

#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"

#include "GGYGOBossDefinition.generated.h"

class UBehaviorTree;
class UGGYGOAbilitySet;
class UGGYGOPawnData;

/** 一个可生成的 Boss 形态。阶段 E 会在此基础上加入 Exit/Enter 配置。 */
USTRUCT(BlueprintType)
struct FGGYGOBossFormDefinition
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta = (Categories = "State.Boss.Form"))
	FGameplayTag FormTag;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	TObjectPtr<const UGGYGOPawnData> AvatarPawnData;
};

/** 一个 Boss 战斗阶段。阶段 B 仅记录身份，不执行阈值切换。 */
USTRUCT(BlueprintType)
struct FGGYGOBossPhaseDefinition
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta = (Categories = "State.Boss.Phase"))
	FGameplayTag PhaseTag;

	/** 进入该阶段的归一化生命阈值；阶段 B 的第一阶段通常为 1。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float EnterHealthThreshold = 1.0f;
};

/**
 * 描述一只 Boss 是什么，不保存运行时状态。
 *
 * 阶段 B 要求 InitialFormTag/InitialPhaseTag 各命中一条配置。全部形态 PawnData 中的
 * AbilitySet 与 PersistentAbilitySets 会在 BossState 初始化时去重后一次性授予。
 */
UCLASS(BlueprintType, Const, meta = (DisplayName = "GGYGO Boss Definition"))
class GGYGO_API UGGYGOBossDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	const FGGYGOBossFormDefinition* FindForm(FGameplayTag FormTag) const;
	const FGGYGOBossPhaseDefinition* FindPhase(FGameplayTag PhaseTag) const;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Initial", meta = (Categories = "State.Boss.Form"))
	FGameplayTag InitialFormTag;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Initial", meta = (Categories = "State.Boss.Phase"))
	FGameplayTag InitialPhaseTag;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Forms", meta = (TitleProperty = "FormTag"))
	TArray<FGGYGOBossFormDefinition> Forms;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Phases", meta = (TitleProperty = "PhaseTag"))
	TArray<FGGYGOBossPhaseDefinition> Phases;

	/** 跨形态持续存在的能力；与各形态 PawnData 的 AbilitySet 合并去重。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Abilities")
	TArray<TObjectPtr<UGGYGOAbilitySet>> PersistentAbilitySets;

	/** 通用 Boss 行为树。阶段 B 允许留空，仅验证 Controller 装配。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI")
	TObjectPtr<UBehaviorTree> BehaviorTree;
};

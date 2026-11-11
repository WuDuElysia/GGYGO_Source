/**
 * @file GGYGOBossDefinition.h
 * @brief Boss 静态定义：形态、阶段动作池与目标查询参数
 */
#pragma once

#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"

#include "GGYGOBossDefinition.generated.h"

class UBehaviorTree;
class UGGYGOAbilitySet;
class UGGYGOBossActionSet;
class UGGYGOPawnData;
class AGGYGOBossState;
class UBTService;

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

/** 一个 Boss 战斗阶段；条件与推进流程由 State 蓝图消费这些数据。 */
USTRUCT(BlueprintType)
struct FGGYGOBossPhaseDefinition
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta = (Categories = "State.Boss.Phase"))
	FGameplayTag PhaseTag;

	/** 阶段蓝图可消费的归一化生命阈值，不在 C++ 中自动决定转阶段。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float EnterHealthThreshold = 1.0f;

	/** 该阶段的可选语义动作；冷却和能否激活仍以 GAS 为真源。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	TObjectPtr<const UGGYGOBossActionSet> ActionSet;
};

/**
 * 描述一只 Boss 是什么，不保存运行时状态。
 *
 * 初始化要求 InitialFormTag/InitialPhaseTag 各命中一条配置。全部形态 PawnData 中的
 * AbilitySet 与 PersistentAbilitySets 会在 BossState 初始化时去重后一次性授予。
 */
UCLASS(BlueprintType, Const, meta = (DisplayName = "GGYGO Boss Definition"))
class GGYGO_API UGGYGOBossDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UGGYGOBossDefinition(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	const FGGYGOBossFormDefinition* FindForm(FGameplayTag FormTag) const;
	const FGGYGOBossPhaseDefinition* FindPhase(FGameplayTag PhaseTag) const;
	/** 校验阶段与战斗树实际消费的配置；初始化和编辑器共用。 */
	bool ValidateCombatConfiguration(FString& OutError) const;
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Initial", meta = (Categories = "State.Boss.Form"))
	FGameplayTag InitialFormTag;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Initial", meta = (Categories = "State.Boss.Phase"))
	FGameplayTag InitialPhaseTag;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Forms", meta = (TitleProperty = "FormTag"))
	TArray<FGGYGOBossFormDefinition> Forms;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Phases", meta = (TitleProperty = "PhaseTag"))
	TArray<FGGYGOBossPhaseDefinition> Phases;

	/** 阶段决策由该 State 蓝图消费生命事实；默认原生类仅维护身份。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Phases")
	TSubclassOf<AGGYGOBossState> BossStateClass;

	/** 跨形态持续存在的能力；与各形态 PawnData 的 AbilitySet 合并去重。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Abilities")
	TArray<TObjectPtr<UGGYGOAbilitySet>> PersistentAbilitySets;

	/** 空树是显式仅装配模式；战斗模式由可编辑 BT 编排。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI")
	TObjectPtr<UBehaviorTree> BehaviorTree;

	/** BT 中实际执行目标保持/重选策略的可编辑服务蓝图。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI")
	TSubclassOf<UBTService> CombatTargetService;


	/** Blackboard 是唯一战斗目标来源，GA 经 Controller 公共接口读取。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI")
	FName CombatTargetKey = TEXT("TargetActor");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "1.0"))
	float TargetAcquireDistance = 4000.0f;

	/** 当前目标可保持到此距离；仅失效后才重新选择最近的存活玩家。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "1.0"))
	float TargetLoseDistance = 5000.0f;

	/** 原生 MoveTo 的停止距离；具体招式距离仍由 ActionSet 决定。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "0.0"))
	float ApproachDistance = 250.0f;
};

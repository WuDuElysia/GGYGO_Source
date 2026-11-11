/** @file GGYGOBossAIController.h @brief 跨 Boss 形态保持的 AIController */
#pragma once

#include "AIController.h"
#include "GameplayTagContainer.h"
#include "GameplayAbilitySpecHandle.h"

#include "GGYGOBossAIController.generated.h"

class AGGYGOBossState;
class UGGYGOBossActionSet;
class UGGYGOBossDefinition;
class UGGYGOAbilitySystemComponent;
struct FGGYGOBossActionDefinition;

/** Result of one synchronous initial assembly call; native Brain owns running state. */
enum class EInitialBossPossessResult : uint8
{
	Failed,
	AssemblyOnly,
	TreeStarted
};

/** 持有 Brain、决策随机流/派生权重与一次性选招请求；战斗目标只保存在 Blackboard。 */
UCLASS(Blueprintable)
class GGYGO_API AGGYGOBossAIController : public AAIController
{
	GENERATED_BODY()

public:
	AGGYGOBossAIController(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	UFUNCTION(BlueprintPure, Category = "GGYGO|Boss")
	AGGYGOBossState* GetBossState() const { return BossState; }

	/** 读取 Blackboard 原目标并核存活事实；具体目标类型、保持与切换由服务蓝图决定。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Boss")
	AActor* GetCombatTarget() const;

	/** 目标策略由 BTService 蓝图决定；此接口只校验并发布 Blackboard 原目标。 */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "GGYGO|Boss")
	bool SetCombatTarget(AActor* Target);

	/** 范围最近点查询，不决定保持/切换目标，也不写 Blackboard。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Boss")
	AActor* FindNearestCombatTarget(float MaxDistance) const;

	/** 死亡入口停止决策和寻路；能力与其资源仍由 GAS 死亡链收尾。 */
	void StopCombatLogic(const TCHAR* Reason);

	/** Encounter 提供可复现的决策种子；仅服务器决策时消费。 */
	void InitializeDecisionStream(int32 EncounterSeed);

	/** Fresh Encounter assembly only. Empty configured tree is an explicit AssemblyOnly mode.
	 * The result belongs to this original call, not a later Possess or a cached ready flag.
	 */
	EInitialBossPossessResult PossessInitialBoss(APawn* ExpectedAvatar, AGGYGOBossState* ExpectedState,
		const UGGYGOBossDefinition* ExpectedDefinition, FString& OutFailure);

	float GetActionWeight(const FGGYGOBossActionDefinition& Action) const;
	double DrawActionWeight(double TotalWeight);
	void RecordActionSelection(const TArray<FGGYGOBossActionDefinition>& Actions, FGameplayTag SelectedActionTag);

	/** 输入为 BT 已通过目标与 GAS 准入的候选；全耗尽只恢复这些候选，再抽签并记录一次。
	 *  ActionSet 须来自 BossState 已采纳并整表校验的 Definition，这里不复验。 */
	const FGGYGOBossActionDefinition* SelectAction(const UGGYGOBossActionSet* ActionSet,
		const TArray<FGameplayTag>& EligibleActionTags);

	/** 只保留一次请求的来源身份；不能作为能力运行状态或冷却来源。 */
	bool StoreActionSelection(const UGGYGOBossActionSet* ActionSet, FGameplayTag PhaseTag,
		UGGYGOAbilitySystemComponent* ASC, FGameplayTag ActionTag, FGameplayAbilitySpecHandle SpecHandle);
	/** 成功或失败均消费旧请求；成功返回选择时的原 SpecHandle，绝不重找另一个 Spec。 */
	bool ConsumeActionSelection(const UGGYGOBossActionSet* ActionSet, FGameplayTag PhaseTag,
		UGGYGOAbilitySystemComponent* ASC, FGameplayTag ActionTag, FGameplayAbilitySpecHandle& OutSpecHandle);
	void ClearActionSelection();

protected:
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "GGYGO|Boss")
	TObjectPtr<AGGYGOBossState> BossState;

	// First-Run / later-Morph-Resume route history; initial readiness comes from native BT facts.
	bool bBehaviorTreeStarted = false;

	FRandomStream DecisionRandom;
	TMap<FGameplayTag, float> RuntimeActionWeights;
	TWeakObjectPtr<const UGGYGOBossActionSet> WeightSource;

private:
	struct FInitialBossPossessContext;
	// Non-owning pointer to the wrapper's stack receipt, installed/restored only by its RAII guard.
	FInitialBossPossessContext* ActiveInitialPossess = nullptr;
	bool IsInitialPossessContextCurrent(const FInitialBossPossessContext& Context) const;
	bool IsInitialPossessTreeCurrent(const FInitialBossPossessContext& Context) const;
	void RetireInitialPossess(const TCHAR* Reason);
	bool IsCombatTargetAlive(const AActor* Target) const;

	// 来源全用弱引用；初始化、重选、消费、附身变化和结束时失效。
	struct FPendingActionSelection
	{
		TWeakObjectPtr<const UGGYGOBossActionSet> ActionSet;
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
		TWeakObjectPtr<UClass> AbilityClass;
		TWeakObjectPtr<AActor> Avatar;
		FGameplayTag PhaseTag;
		FGameplayTag ActionTag;
		FGameplayAbilitySpecHandle SpecHandle;
	};
	FPendingActionSelection PendingActionSelection;
};

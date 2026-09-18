/** @file GGYGOBossAIController.h @brief 跨 Boss 形态保持的 AIController */
#pragma once

#include "AIController.h"

#include "GGYGOBossAIController.generated.h"

class AGGYGOBossState;
struct FGGYGOBossActionDefinition;

/** 阶段 B 只负责 Possess 与启动 BehaviorTree；目标/仇恨在阶段 D 加入。 */
UCLASS(Blueprintable)
class GGYGO_API AGGYGOBossAIController : public AAIController
{
	GENERATED_BODY()

public:
	AGGYGOBossAIController(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	UFUNCTION(BlueprintPure, Category = "GGYGO|Boss")
	AGGYGOBossState* GetBossState() const { return BossState; }

	/** Encounter 提供可复现的决策种子；仅服务器决策时消费。 */
	void InitializeDecisionStream(int32 EncounterSeed);

	float GetActionWeight(const FGGYGOBossActionDefinition& Action) const;
	float DrawActionWeight(float TotalWeight);
	void RecordActionSelection(const TArray<FGGYGOBossActionDefinition>& Actions, FGameplayTag SelectedActionTag);

protected:
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "GGYGO|Boss")
	TObjectPtr<AGGYGOBossState> BossState;

	bool bBehaviorTreeStarted = false;

	FRandomStream DecisionRandom;
	TMap<FGameplayTag, float> RuntimeActionWeights;
};

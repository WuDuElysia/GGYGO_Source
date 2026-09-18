/** @file BTTask_GGYGOChooseBossAction.h @brief 过滤并选择一个可激活的 Boss Action */
#pragma once

#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTreeTypes.h"

#include "BTTask_GGYGOChooseBossAction.generated.h"

UCLASS(meta = (DisplayName = "GGYGO Choose Boss Action"))
class GGYGO_API UBTTask_GGYGOChooseBossAction : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_GGYGOChooseBossAction(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

protected:
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;

	/** 可选。阶段 D 的 CombatTarget 就位前，无目标动作仍可被选中。 */
	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector TargetActorKey;

	/** 写入 ActionTag 的 FName，不把冷却或状态镜像进 Blackboard。 */
	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector SelectedActionKey;
};

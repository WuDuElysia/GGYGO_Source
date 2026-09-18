/** @file BTTask_GGYGOActivateAbility.h @brief 激活被选中的语义动作并精确等待该 Spec 结束 */
#pragma once

#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "GameplayAbilitySpecHandle.h"

#include "BTTask_GGYGOActivateAbility.generated.h"

class UGGYGOAbilitySystemComponent;
struct FAbilityEndedData;

UCLASS(meta = (DisplayName = "GGYGO Activate Boss Ability"))
class GGYGO_API UBTTask_GGYGOActivateAbility : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_GGYGOActivateAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

protected:
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory,
		EBTNodeResult::Type TaskResult) override;

	void HandleAbilityEnded(const FAbilityEndedData& EndedData);
	void CleanupBinding();

	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector SelectedActionKey;

	TWeakObjectPtr<UGGYGOAbilitySystemComponent> WaitingASC;
	TWeakObjectPtr<UBehaviorTreeComponent> ActiveOwnerComp;
	FGameplayAbilitySpecHandle WaitingHandle;
	bool bInsideTryActivate = false;
	bool bEndedDuringActivation = false;
	bool bEndedDuringActivationWasCancelled = false;
};

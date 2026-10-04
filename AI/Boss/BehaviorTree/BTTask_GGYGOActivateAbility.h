/** @file BTTask_GGYGOActivateAbility.h @brief 请求被选动作并精确等待原 Activation 终止完成 */
#pragma once

#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "GameplayAbilitySpecHandle.h"

#include "BTTask_GGYGOActivateAbility.generated.h"

class UGGYGOAbilitySystemComponent;
class FGGYGOAbilityTerminationCompletedNotice;

UCLASS(meta = (DisplayName = "GGYGO Activate Boss Ability"))
class GGYGO_API UBTTask_GGYGOActivateAbility : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_GGYGOActivateAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

protected:
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void BeginDestroy() override;

	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector SelectedActionKey;

private:
	struct FAbilityWait;
	// Only this execution's subscription and copied provenance; GAS owns lifecycle/completion.
	TSharedPtr<FAbilityWait> ActiveWait;
	// Non-owning last wait detects local replacement after detachment; it is not native BT provenance.
	TWeakPtr<FAbilityWait> LastWait;

	bool IsWaitCurrent(const TSharedPtr<FAbilityWait>& Wait) const;
	void HandleTerminationCompleted(const FGGYGOAbilityTerminationCompletedNotice& Notice,
		const TSharedPtr<FAbilityWait>& Wait);
	void CleanupBinding(TSharedPtr<FAbilityWait> Wait);
};

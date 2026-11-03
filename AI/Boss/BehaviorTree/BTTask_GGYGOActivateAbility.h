/** @file BTTask_GGYGOActivateAbility.h @brief 请求被选动作，并等待该 Spec 这次激活的原生结束 */
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
	virtual void BeginDestroy() override;

	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector SelectedActionKey;

private:
	struct FAbilityWait;
	// 只保存本次执行的订阅与 Spec；激活与结束由 GAS 拥有。
	TSharedPtr<FAbilityWait> ActiveWait;
	// Non-owning last wait detects local replacement after detachment; it is not native BT provenance.
	TWeakPtr<FAbilityWait> LastWait;

	bool IsWaitCurrent(const TSharedPtr<FAbilityWait>& Wait) const;
	void HandleAbilityEnded(const FAbilityEndedData& Data, const TSharedPtr<FAbilityWait>& Wait);
	void CleanupBinding(TSharedPtr<FAbilityWait> Wait);
};

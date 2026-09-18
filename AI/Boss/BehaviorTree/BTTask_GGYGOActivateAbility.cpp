/** @file BTTask_GGYGOActivateAbility.cpp */
#include "AI/Boss/BehaviorTree/BTTask_GGYGOActivateAbility.h"

#include "AI/Boss/GGYGOBossAIController.h"
#include "AI/Boss/GGYGOBossActionSet.h"
#include "AI/Boss/GGYGOBossDefinition.h"
#include "AI/Boss/GGYGOBossState.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(BTTask_GGYGOActivateAbility)

namespace
{
	const UGGYGOBossActionSet* ResolveActionSet(const AGGYGOBossState* BossState)
	{
		const UGGYGOBossDefinition* Definition = BossState ? BossState->GetBossDefinition() : nullptr;
		const FGGYGOBossPhaseDefinition* Phase = Definition
			? Definition->FindPhase(BossState->GetCurrentPhaseTag())
			: nullptr;
		return Phase ? Phase->ActionSet : nullptr;
	}
}

UBTTask_GGYGOActivateAbility::UBTTask_GGYGOActivateAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NodeName = TEXT("Activate Boss Ability");
	bCreateNodeInstance = true;
	bNotifyTaskFinished = true;
	SelectedActionKey.AddNameFilter(this, GET_MEMBER_NAME_CHECKED(ThisClass, SelectedActionKey));
	SelectedActionKey.SelectedKeyName = TEXT("SelectedAction");
}

EBTNodeResult::Type UBTTask_GGYGOActivateAbility::ExecuteTask(
	UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AGGYGOBossAIController* Controller = Cast<AGGYGOBossAIController>(OwnerComp.GetAIOwner());
	AGGYGOBossState* BossState = Controller ? Controller->GetBossState() : nullptr;
	UGGYGOAbilitySystemComponent* ASC = BossState ? BossState->GetGGYGOAbilitySystemComponent() : nullptr;
	const UGGYGOBossActionSet* ActionSet = ResolveActionSet(BossState);
	const UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (!ASC || !ActionSet || !Blackboard)
	{
		return EBTNodeResult::Failed;
	}

	const FName ActionName = Blackboard->GetValueAsName(SelectedActionKey.SelectedKeyName);
	const FGameplayTag ActionTag = FGameplayTag::RequestGameplayTag(ActionName, false);
	const FGGYGOBossActionDefinition* Action = ActionSet->FindAction(ActionTag);
	FGameplayAbilitySpec* Spec = Action && Action->AbilityClass
		? ASC->FindAbilitySpecFromClass(Action->AbilityClass)
		: nullptr;
	if (!Spec)
	{
		return EBTNodeResult::Failed;
	}

	WaitingASC = ASC;
	ActiveOwnerComp = &OwnerComp;
	WaitingHandle = Spec->Handle;
	bEndedDuringActivation = false;
	bEndedDuringActivationWasCancelled = false;
	ASC->OnAbilityEnded.AddUObject(this, &ThisClass::HandleAbilityEnded);

	bInsideTryActivate = true;
	const bool bActivated = ASC->TryActivateAbility(WaitingHandle);
	bInsideTryActivate = false;

	if (!bActivated)
	{
		CleanupBinding();
		return EBTNodeResult::Failed;
	}
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("ActivateBossAbility: [%s] 已激活 [%s]，等待 Spec [%s] 结束。"),
		*GetNameSafe(Controller), *ActionTag.ToString(), *WaitingHandle.ToString());
	if (bEndedDuringActivation)
	{
		const bool bWasCancelled = bEndedDuringActivationWasCancelled;
		CleanupBinding();
		return bWasCancelled ? EBTNodeResult::Failed : EBTNodeResult::Succeeded;
	}
	return EBTNodeResult::InProgress;
}

EBTNodeResult::Type UBTTask_GGYGOActivateAbility::AbortTask(
	UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	// Abort 只停止等待，不默认取消可能已进入不可取消段的 GA。
	CleanupBinding();
	return EBTNodeResult::Aborted;
}

void UBTTask_GGYGOActivateAbility::OnTaskFinished(
	UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult)
{
	CleanupBinding();
	Super::OnTaskFinished(OwnerComp, NodeMemory, TaskResult);
}

void UBTTask_GGYGOActivateAbility::HandleAbilityEnded(const FAbilityEndedData& EndedData)
{
	if (EndedData.AbilitySpecHandle != WaitingHandle)
	{
		return;
	}

	if (bInsideTryActivate)
	{
		bEndedDuringActivation = true;
		bEndedDuringActivationWasCancelled = EndedData.bWasCancelled;
		return;
	}

	UBehaviorTreeComponent* OwnerComp = ActiveOwnerComp.Get();
	const EBTNodeResult::Type Result = EndedData.bWasCancelled
		? EBTNodeResult::Failed
		: EBTNodeResult::Succeeded;
	CleanupBinding();
	if (OwnerComp)
	{
		FinishLatentTask(*OwnerComp, Result);
	}
}

void UBTTask_GGYGOActivateAbility::CleanupBinding()
{
	if (UGGYGOAbilitySystemComponent* ASC = WaitingASC.Get())
	{
		ASC->OnAbilityEnded.RemoveAll(this);
	}
	WaitingASC.Reset();
	ActiveOwnerComp.Reset();
	WaitingHandle = FGameplayAbilitySpecHandle();
}

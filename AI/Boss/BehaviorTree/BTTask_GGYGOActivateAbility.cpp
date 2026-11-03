/** @file BTTask_GGYGOActivateAbility.cpp */
#include "AI/Boss/BehaviorTree/BTTask_GGYGOActivateAbility.h"

#include "AI/Boss/GGYGOBossAIController.h"
#include "AI/Boss/GGYGOBossActionSet.h"
#include "AI/Boss/GGYGOBossDefinition.h"
#include "AI/Boss/GGYGOBossState.h"
#include "AbilitySystem/Abilities/GGYGOCombatActionAbility.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "GameFramework/Pawn.h"

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

struct UBTTask_GGYGOActivateAbility::FAbilityWait
{
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
	TWeakObjectPtr<UBehaviorTreeComponent> OwnerComp;
	FGameplayAbilitySpecHandle Spec;
	FDelegateHandle EndedBinding;
	/** TryActivateAbility 调用栈内为真；期间的结束只记录，由 ExecuteTask 的返回值交付。 */
	bool bTryInProgress = false;
	bool bEndedDuringTry = false;
	bool bWasCancelled = false;
};

UBTTask_GGYGOActivateAbility::UBTTask_GGYGOActivateAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NodeName = TEXT("Activate Boss Ability");
	bCreateNodeInstance = true;
	// Native OnTaskFinished carries no original execution identity after its parent callback.
	// Each owned wait is released at failure/completion/Abort/destruction, before native finish.
	bNotifyTaskFinished = false;
	SelectedActionKey.AddNameFilter(this, GET_MEMBER_NAME_CHECKED(ThisClass, SelectedActionKey));
	SelectedActionKey.SelectedKeyName = TEXT("SelectedAction");
}

EBTNodeResult::Type UBTTask_GGYGOActivateAbility::ExecuteTask(
	UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	TSharedPtr<FAbilityWait> PreviousWait = MoveTemp(ActiveWait);
	const TSharedPtr<FAbilityWait> Wait = MakeShared<FAbilityWait>();
	Wait->OwnerComp = &OwnerComp;
	ActiveWait = Wait;
	LastWait = Wait;
	CleanupBinding(MoveTemp(PreviousWait));
	const auto FailExecution = [this, &Wait]()
	{
		const bool bOriginalWait = IsWaitCurrent(Wait);
		CleanupBinding(Wait);
		return bOriginalWait ? EBTNodeResult::Failed : EBTNodeResult::Aborted;
	};
	if (!IsWaitCurrent(Wait)) { return FailExecution(); }

	AGGYGOBossAIController* Controller = Cast<AGGYGOBossAIController>(OwnerComp.GetAIOwner());
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (!Controller)
	{
		if (Blackboard)
		{
			Blackboard->ClearValue(SelectedActionKey.SelectedKeyName);
		}
		return FailExecution();
	}

	FGameplayTag ActionTag;
	if (Blackboard)
	{
		const FName ActionName = Blackboard->GetValueAsName(SelectedActionKey.SelectedKeyName);
		ActionTag = FGameplayTag::RequestGameplayTag(ActionName, false);
	}
	AGGYGOBossState* BossState = Controller->GetBossState();
	UGGYGOAbilitySystemComponent* ASC = BossState ? BossState->GetGGYGOAbilitySystemComponent() : nullptr;
	const UGGYGOBossActionSet* ActionSet = ResolveActionSet(BossState);
	const FGameplayTag PhaseTag = BossState ? BossState->GetCurrentPhaseTag() : FGameplayTag();
	APawn* Pawn = Controller->GetPawn();
	FGameplayAbilitySpecHandle SelectedHandle;
	const bool bConsumedSelection =
		Controller->ConsumeActionSelection(ActionSet, PhaseTag, ASC, ActionTag, SelectedHandle);
	if (!IsWaitCurrent(Wait)) { return FailExecution(); }
	if (Blackboard)
	{
		Blackboard->ClearValue(SelectedActionKey.SelectedKeyName);
	}
	if (!IsWaitCurrent(Wait)) { return FailExecution(); }

	const auto IsActionSourceCurrent = [&]()
	{
		return IsWaitCurrent(Wait) && IsValid(Blackboard) && bConsumedSelection && IsValid(Controller)
			&& Cast<AGGYGOBossAIController>(OwnerComp.GetAIOwner()) == Controller
			&& IsValid(BossState) && Controller->GetBossState() == BossState
			&& IsValid(ASC) && BossState->GetGGYGOAbilitySystemComponent() == ASC
			&& IsValid(ActionSet) && ResolveActionSet(BossState) == ActionSet
			&& ActionTag.IsValid() && PhaseTag.IsValid() && BossState->GetCurrentPhaseTag() == PhaseTag
			&& IsValid(Pawn) && Controller->GetPawn() == Pawn && ASC->GetAvatarActor() == Pawn;
	};
	if (!IsActionSourceCurrent())
	{
		return FailExecution();
	}

	// Clearing Blackboard can synchronously notify observers. If one chose a newer action,
	// preserve its request and let this consumed choice fail without activating alongside it.
	if (Blackboard->GetValueAsName(SelectedActionKey.SelectedKeyName) != NAME_None)
	{
		return FailExecution();
	}

	// ActionSet 已在 BossState 采纳 Definition 时整表校验；这里只核对被消费的动作行与 Spec。
	const FGGYGOBossActionDefinition* Action = ActionSet->FindAction(ActionTag);
	const FGameplayAbilitySpec* SelectedSpec = ASC->FindAbilitySpecFromHandle(SelectedHandle);
	UClass* AbilityClass = Action ? Action->AbilityClass.Get() : nullptr;
	if (!SelectedHandle.IsValid() || !Action || Action->BaseWeight <= 0.0f ||
		!IsValid(AbilityClass) || !SelectedSpec || !SelectedSpec->Ability ||
		SelectedSpec->Ability->GetClass() != AbilityClass)
	{
		return FailExecution();
	}
	const UGGYGOCombatActionAbility* AbilityCDO =
		AbilityClass->GetDefaultObject<UGGYGOCombatActionAbility>();
	if (!AbilityCDO || AbilityCDO->GetActionTag() != ActionTag)
	{
		return FailExecution();
	}

	if (!IsActionSourceCurrent() || Blackboard->GetValueAsName(SelectedActionKey.SelectedKeyName) != NAME_None)
	{
		return FailExecution();
	}
	Wait->ASC = ASC;
	Wait->Spec = SelectedHandle;
	const TWeakPtr<FAbilityWait> OriginalWait = Wait;
	Wait->EndedBinding = ASC->OnAbilityEnded.AddWeakLambda(this,
		[this, OriginalWait](const FAbilityEndedData& Data)
		{
			if (const TSharedPtr<FAbilityWait> PinnedWait = OriginalWait.Pin())
			{
				HandleAbilityEnded(Data, PinnedWait);
			}
		});
	const FString OriginalASCName = GetNameSafe(ASC);
	const FString OriginalActionName = ActionTag.ToString();
	Wait->bTryInProgress = true;
	const bool bActivated = ASC->TryActivateAbility(SelectedHandle);
	Wait->bTryInProgress = false;
	if (!IsWaitCurrent(Wait)) { return FailExecution(); }
	if (!bActivated)
	{
		UE_LOG(LogGGYGOAbilitySystem, Verbose,
			TEXT("ActivateBossAbility: ASC [%s] Action [%s] Spec [%s] 激活未被接受。"),
			*OriginalASCName, *OriginalActionName, *SelectedHandle.ToString());
		return FailExecution();
	}
	if (Wait->bEndedDuringTry)
	{
		const bool bWasCancelled = Wait->bWasCancelled;
		CleanupBinding(Wait);
		return bWasCancelled ? EBTNodeResult::Failed : EBTNodeResult::Succeeded;
	}
	const FGameplayAbilitySpec* ActivatedSpec = ASC->FindAbilitySpecFromHandle(SelectedHandle);
	if (!ActivatedSpec || !ActivatedSpec->IsActive())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("ActivateBossAbility: ASC [%s] Action [%s] Spec [%s] 原生请求已接受，但本地没有可等待的运行中激活。"),
			*OriginalASCName, *OriginalActionName, *SelectedHandle.ToString());
		return FailExecution();
	}
	return EBTNodeResult::InProgress;
}

EBTNodeResult::Type UBTTask_GGYGOActivateAbility::AbortTask(
	UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	// Abort 只停止等待，不默认取消可能已进入不可取消段的 GA。
	const TSharedPtr<FAbilityWait> Wait = ActiveWait;
	if (Wait && Wait->OwnerComp.Get() == &OwnerComp)
	{
		if (LastWait.Pin() == Wait) { LastWait.Reset(); }
		CleanupBinding(Wait);
	}
	return EBTNodeResult::Aborted;
}

void UBTTask_GGYGOActivateAbility::BeginDestroy()
{
	LastWait.Reset();
	CleanupBinding(ActiveWait);
	Super::BeginDestroy();
}

bool UBTTask_GGYGOActivateAbility::IsWaitCurrent(const TSharedPtr<FAbilityWait>& Wait) const
{
	const UBehaviorTreeComponent* OwnerComp = Wait ? Wait->OwnerComp.Get() : nullptr;
	return Wait && ActiveWait == Wait && LastWait.Pin() == Wait
		&& IsValid(this) && !HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		&& IsValid(OwnerComp) && !OwnerComp->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed);
}

void UBTTask_GGYGOActivateAbility::HandleAbilityEnded(
	const FAbilityEndedData& Data, const TSharedPtr<FAbilityWait>& Wait)
{
	// 项目能力 InstancedPerActor 且禁止重触发，同一 Spec 同时只有一次激活，按 Spec 匹配即可。
	if (!IsWaitCurrent(Wait) || Data.AbilitySpecHandle != Wait->Spec)
	{
		return;
	}
	if (Wait->bTryInProgress)
	{
		Wait->bEndedDuringTry = true;
		Wait->bWasCancelled = Data.bWasCancelled;
		return;
	}
	const TWeakObjectPtr<UBehaviorTreeComponent> OriginalOwner = Wait->OwnerComp;
	const EBTNodeResult::Type Result = Data.bWasCancelled
		? EBTNodeResult::Failed
		: EBTNodeResult::Succeeded;
	CleanupBinding(Wait);
	UBehaviorTreeComponent* OwnerComp = OriginalOwner.Get();
	if (IsValid(this) && !HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		&& LastWait.Pin() == Wait && IsValid(OwnerComp)
		&& !OwnerComp->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		FinishLatentTask(*OwnerComp, Result);
		// No member writes or wait/successor cleanup after the native completion call.
	}
}

void UBTTask_GGYGOActivateAbility::CleanupBinding(TSharedPtr<FAbilityWait> Wait)
{
	if (!Wait) { return; }
	if (ActiveWait == Wait) { ActiveWait.Reset(); }
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC = Wait->ASC;
	const FDelegateHandle Binding = Wait->EndedBinding;
	Wait->EndedBinding.Reset();
	Wait->ASC.Reset();
	Wait->OwnerComp.Reset();
	Wait->Spec = {};
	if (UGGYGOAbilitySystemComponent* ASC = OriginalASC.Get(); ASC && Binding.IsValid())
	{
		ASC->OnAbilityEnded.Remove(Binding);
	}
}

/** @file BTTask_GGYGOChooseBossAction.cpp */
#include "AI/Boss/BehaviorTree/BTTask_GGYGOChooseBossAction.h"

#include "AI/Boss/GGYGOBossAIController.h"
#include "AI/Boss/GGYGOBossActionSet.h"
#include "AI/Boss/GGYGOBossDefinition.h"
#include "AI/Boss/GGYGOBossState.h"
#include "AbilitySystem/Abilities/GGYGOCombatActionAbility.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "GameFramework/Pawn.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(BTTask_GGYGOChooseBossAction)

namespace
{
	const UGGYGOBossActionSet* ResolveCurrentActionSet(const AGGYGOBossState* BossState)
	{
		const UGGYGOBossDefinition* Definition = BossState ? BossState->GetBossDefinition() : nullptr;
		const FGGYGOBossPhaseDefinition* Phase = Definition
			? Definition->FindPhase(BossState->GetCurrentPhaseTag())
			: nullptr;
		return Phase ? Phase->ActionSet : nullptr;
	}
}

UBTTask_GGYGOChooseBossAction::UBTTask_GGYGOChooseBossAction(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NodeName = TEXT("Choose Boss Action");
	TargetActorKey.AddObjectFilter(this, GET_MEMBER_NAME_CHECKED(ThisClass, TargetActorKey), AActor::StaticClass());
	SelectedActionKey.AddNameFilter(this, GET_MEMBER_NAME_CHECKED(ThisClass, SelectedActionKey));
	TargetActorKey.SelectedKeyName = TEXT("TargetActor");
	SelectedActionKey.SelectedKeyName = TEXT("SelectedAction");
}

EBTNodeResult::Type UBTTask_GGYGOChooseBossAction::ExecuteTask(
	UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AGGYGOBossAIController* Controller = Cast<AGGYGOBossAIController>(OwnerComp.GetAIOwner());
	AGGYGOBossState* BossState = Controller ? Controller->GetBossState() : nullptr;
	UGGYGOAbilitySystemComponent* ASC = BossState ? BossState->GetGGYGOAbilitySystemComponent() : nullptr;
	const UGGYGOBossActionSet* ActionSet = ResolveCurrentActionSet(BossState);
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	APawn* Avatar = Controller ? Controller->GetPawn() : nullptr;
	if (!Controller || !ASC || !ActionSet || !Blackboard || !Avatar)
	{
		return EBTNodeResult::Failed;
	}

	AActor* Target = Cast<AActor>(Blackboard->GetValueAsObject(TargetActorKey.SelectedKeyName));
	FGameplayTagContainer OwnedTags;
	ASC->GetOwnedGameplayTags(OwnedTags);

	struct FCandidate
	{
		const FGGYGOBossActionDefinition* Action = nullptr;
		float Weight = 0.0f;
	};
	TArray<FCandidate> Candidates;
	float TotalWeight = 0.0f;

	for (const FGGYGOBossActionDefinition& Action : ActionSet->Actions)
	{
		if (!Action.ActionTag.IsValid() || !Action.AbilityClass || Action.BaseWeight <= 0.0f ||
			!OwnedTags.HasAll(Action.RequiredTags) || OwnedTags.HasAny(Action.BlockedTags))
		{
			continue;
		}

		const UGGYGOCombatActionAbility* AbilityCDO = Action.AbilityClass->GetDefaultObject<UGGYGOCombatActionAbility>();
		if (!AbilityCDO || AbilityCDO->GetActionTag() != Action.ActionTag)
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("ChooseBossAction: [%s] 的 ActionTag 与 Ability [%s] 不一致。"),
				*Action.ActionTag.ToString(), *GetNameSafe(Action.AbilityClass));
			continue;
		}

		if (Target)
		{
			const FVector ToTarget = Target->GetActorLocation() - Avatar->GetActorLocation();
			const float Distance = ToTarget.Size();
			if (Distance < Action.MinDistance || (Action.MaxDistance > 0.0f && Distance > Action.MaxDistance))
			{
				continue;
			}
			if (!ToTarget.IsNearlyZero())
			{
				const float FacingDot = FVector::DotProduct(Avatar->GetActorForwardVector(), ToTarget.GetSafeNormal());
				const float FacingAngle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FacingDot, -1.0f, 1.0f)));
				if (FacingAngle > Action.MaxFacingAngle)
				{
					continue;
				}
			}
			if (Action.bRequiresLineOfSight && !Controller->LineOfSightTo(Target))
			{
				continue;
			}
		}
		else if (Action.MinDistance > 0.0f || Action.MaxDistance > 0.0f || Action.bRequiresLineOfSight)
		{
			continue;
		}

		const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromClass(Action.AbilityClass);
		FGameplayTagContainer FailureTags;
		if (!Spec || !ASC->CanActivateAbilityByHandle(Spec->Handle, FailureTags))
		{
			continue;
		}

		const float Weight = Controller->GetActionWeight(Action);
		if (Weight > 0.0f)
		{
			Candidates.Add({ &Action, Weight });
			TotalWeight += Weight;
		}
	}

	if (Candidates.IsEmpty() || TotalWeight <= 0.0f)
	{
		Blackboard->ClearValue(SelectedActionKey.SelectedKeyName);
		return EBTNodeResult::Failed;
	}

	const float Roll = Controller->DrawActionWeight(TotalWeight);
	float AccumulatedWeight = 0.0f;
	const FGGYGOBossActionDefinition* Selected = Candidates.Last().Action;
	for (const FCandidate& Candidate : Candidates)
	{
		AccumulatedWeight += Candidate.Weight;
		if (Roll <= AccumulatedWeight)
		{
			Selected = Candidate.Action;
			break;
		}
	}

	Blackboard->SetValueAsName(SelectedActionKey.SelectedKeyName, Selected->ActionTag.GetTagName());
	Controller->RecordActionSelection(ActionSet->Actions, Selected->ActionTag);
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("ChooseBossAction: [%s] 选择 [%s]，候选数 [%d]。"),
		*GetNameSafe(Controller), *Selected->ActionTag.ToString(), Candidates.Num());
	return EBTNodeResult::Succeeded;
}

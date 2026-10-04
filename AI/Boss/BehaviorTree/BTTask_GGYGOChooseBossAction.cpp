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
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (Controller)
	{
		Controller->ClearActionSelection();
	}
	if (Blackboard)
	{
		Blackboard->ClearValue(SelectedActionKey.SelectedKeyName);
	}

	const auto Fail = [&Controller, &Blackboard, this]()
	{
		if (Controller)
		{
			Controller->ClearActionSelection();
		}
		if (Blackboard)
		{
			Blackboard->ClearValue(SelectedActionKey.SelectedKeyName);
		}
		return EBTNodeResult::Failed;
	};

	AGGYGOBossState* BossState = Controller ? Controller->GetBossState() : nullptr;
	UGGYGOAbilitySystemComponent* ASC = BossState ? BossState->GetGGYGOAbilitySystemComponent() : nullptr;
	const FGameplayTag PhaseTag = BossState ? BossState->GetCurrentPhaseTag() : FGameplayTag();
	const UGGYGOBossActionSet* ActionSet = ResolveCurrentActionSet(BossState);
	APawn* Avatar = Controller ? Controller->GetPawn() : nullptr;
	if (!Controller || !ASC || !ActionSet || !Blackboard || !PhaseTag.IsValid() || !IsValid(Avatar) ||
		ASC->GetAvatarActor() != Avatar)
	{
		return Fail();
	}

	FString ValidationError;
	if (!ActionSet->ValidateConfiguration(ValidationError))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("ChooseBossAction: 拒绝无效 ActionSet [%s]：%s"),
			*GetNameSafe(ActionSet), *ValidationError);
		return Fail();
	}

	AActor* Target = Cast<AActor>(Blackboard->GetValueAsObject(TargetActorKey.SelectedKeyName));
	FGameplayTagContainer OwnedTags;
	ASC->GetOwnedGameplayTags(OwnedTags);

	struct FCandidate
	{
		FGameplayTag ActionTag;
		FGameplayAbilitySpecHandle SpecHandle;
	};
	TArray<FCandidate> Candidates;
	TArray<FGameplayTag> EligibleActionTags;

	for (const FGGYGOBossActionDefinition& Action : ActionSet->Actions)
	{
		if (Action.BaseWeight <= 0.0f ||
			!OwnedTags.HasAll(Action.RequiredTags) || OwnedTags.HasAny(Action.BlockedTags))
		{
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
		if (!Spec)
		{
			continue;
		}

		// The admission query may invoke code that changes the ability list. Keep only the
		// original handle after this point; never dereference Spec again.
		const FGameplayTag CandidateTag = Action.ActionTag;
		const FGameplayAbilitySpecHandle CandidateHandle = Spec->Handle;
		if (!CandidateHandle.IsValid())
		{
			continue;
		}

		FGameplayTagContainer FailureTags;
		if (!ASC->CanActivateAbilityByHandle(CandidateHandle, FailureTags))
		{
			continue;
		}

		Candidates.Add({ CandidateTag, CandidateHandle });
		EligibleActionTags.Add(CandidateTag);
	}

	if (Candidates.IsEmpty())
	{
		return EBTNodeResult::Failed;
	}

	const auto IsSelectionSourceCurrent = [&OwnerComp, Controller, BossState, ASC, ActionSet, Avatar, PhaseTag]()
	{
		if (!IsValid(Controller) || Cast<AGGYGOBossAIController>(OwnerComp.GetAIOwner()) != Controller ||
			!IsValid(BossState) || Controller->GetBossState() != BossState || !IsValid(ASC) ||
			BossState->GetGGYGOAbilitySystemComponent() != ASC || !IsValid(ActionSet) ||
			!IsValid(Avatar) || Controller->GetPawn() != Avatar || ASC->GetAvatarActor() != Avatar)
		{
			return false;
		}
		return BossState->GetCurrentPhaseTag() == PhaseTag &&
			ResolveCurrentActionSet(BossState) == ActionSet;
	};
	if (!IsSelectionSourceCurrent())
	{
		return EBTNodeResult::Failed;
	}

	const FGGYGOBossActionDefinition* Selected = Controller->SelectAction(ActionSet, EligibleActionTags);
	if (!Selected)
	{
		return EBTNodeResult::Failed;
	}
	if (!IsSelectionSourceCurrent())
	{
		return EBTNodeResult::Failed;
	}

	FGameplayAbilitySpecHandle SelectedHandle;
	for (const FCandidate& Candidate : Candidates)
	{
		if (Candidate.ActionTag == Selected->ActionTag)
		{
			SelectedHandle = Candidate.SpecHandle;
			break;
		}
	}
	if (!SelectedHandle.IsValid() ||
		!Controller->StoreActionSelection(ActionSet, PhaseTag,
			ASC, Selected->ActionTag, SelectedHandle))
	{
		return EBTNodeResult::Failed;
	}

	Blackboard->SetValueAsName(SelectedActionKey.SelectedKeyName, Selected->ActionTag.GetTagName());
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("ChooseBossAction: [%s] 选择 [%s]，候选数 [%d]。"),
		*GetNameSafe(Controller), *Selected->ActionTag.ToString(), Candidates.Num());
	return EBTNodeResult::Succeeded;
}

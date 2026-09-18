/** @file GGYGOBossAIController.cpp */
#include "AI/Boss/GGYGOBossAIController.h"

#include "AI/Boss/GGYGOBossDefinition.h"
#include "AI/Boss/GGYGOBossActionSet.h"
#include "AI/Boss/GGYGOBossState.h"
#include "AbilitySystemInterface.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BrainComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossAIController)

AGGYGOBossAIController::AGGYGOBossAIController(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;
	bWantsPlayerState = false;

	// 形态交接只暂停 Brain，不能走 AAIController 默认的 Cleanup；否则黑板与 BT 实例会被重建。
	bStopAILogicOnUnposses = false;
	bStartAILogicOnPossess = true;
	DecisionRandom.Initialize(0);
}

void AGGYGOBossAIController::InitializeDecisionStream(int32 EncounterSeed)
{
	DecisionRandom.Initialize(EncounterSeed);
	RuntimeActionWeights.Reset();
}

float AGGYGOBossAIController::GetActionWeight(const FGGYGOBossActionDefinition& Action) const
{
	if (const float* RuntimeWeight = RuntimeActionWeights.Find(Action.ActionTag))
	{
		return *RuntimeWeight;
	}
	return FMath::Max(Action.BaseWeight, 0.0f);
}

float AGGYGOBossAIController::DrawActionWeight(float TotalWeight)
{
	return DecisionRandom.FRandRange(0.0f, FMath::Max(TotalWeight, 0.0f));
}

void AGGYGOBossAIController::RecordActionSelection(
	const TArray<FGGYGOBossActionDefinition>& Actions, FGameplayTag SelectedActionTag)
{
	for (const FGGYGOBossActionDefinition& Action : Actions)
	{
		const float CurrentWeight = GetActionWeight(Action);
		const float NewWeight = Action.ActionTag == SelectedActionTag
			? CurrentWeight * Action.RepeatPenalty
			: CurrentWeight + Action.UnusedWeightGain;
		RuntimeActionWeights.FindOrAdd(Action.ActionTag) = FMath::Clamp(
			NewWeight, 0.0f, FMath::Max(Action.MaxWeight, Action.BaseWeight));
	}
}

void AGGYGOBossAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	AGGYGOBossState* ResolvedState = nullptr;
	if (const IAbilitySystemInterface* AbilityInterface = Cast<IAbilitySystemInterface>(InPawn))
	{
		if (UAbilitySystemComponent* ASC = AbilityInterface->GetAbilitySystemComponent())
		{
			ResolvedState = Cast<AGGYGOBossState>(ASC->GetOwnerActor());
		}
	}

	if (!ResolvedState)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossAIController [%s] Possess [%s] 时未找到 BossState ASC Owner。"),
			*GetNameSafe(this), *GetNameSafe(InPawn));
		return;
	}

	BossState = ResolvedState;
	const UGGYGOBossDefinition* Definition = BossState->GetBossDefinition();
	if (!bBehaviorTreeStarted && Definition && Definition->BehaviorTree)
	{
		bBehaviorTreeStarted = RunBehaviorTree(Definition->BehaviorTree);
		if (!bBehaviorTreeStarted)
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("BossAIController [%s] 无法启动行为树 [%s]。"),
				*GetNameSafe(this), *GetNameSafe(Definition->BehaviorTree));
		}
		else
		{
			UE_LOG(LogGGYGOAbilitySystem, Display,
				TEXT("BossAIController [%s] 已启动行为树 [%s]。"),
				*GetNameSafe(this), *GetNameSafe(Definition->BehaviorTree));
		}
	}
	else if (bBehaviorTreeStarted)
	{
		if (UBrainComponent* Brain = GetBrainComponent())
		{
			Brain->ResumeLogic(TEXT("Boss Avatar ready"));
		}
	}
}

void AGGYGOBossAIController::OnUnPossess()
{
	if (UBrainComponent* Brain = GetBrainComponent())
	{
		Brain->PauseLogic(TEXT("Boss Avatar handoff"));
	}

	// BossState 与 Brain 都保留；阶段 E 的新形态 Possess 后继续使用同一 Controller。
	Super::OnUnPossess();
}

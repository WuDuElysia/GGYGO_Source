/** @file GGYGOBossAIController.cpp */
#include "AI/Boss/GGYGOBossAIController.h"

#include "AI/Boss/GGYGOBossDefinition.h"
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

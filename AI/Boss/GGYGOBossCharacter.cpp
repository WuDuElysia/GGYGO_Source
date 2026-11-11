/** @file GGYGOBossCharacter.cpp */
#include "AI/Boss/GGYGOBossCharacter.h"

#include "AI/Boss/GGYGOBossAIController.h"
#include "Combat/HitDetection/GGYGOMeleeTraceComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossCharacter)

AGGYGOBossCharacter::AGGYGOBossCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	AIControllerClass = AGGYGOBossAIController::StaticClass();
	MeleeTraceComponent = CreateDefaultSubobject<UGGYGOMeleeTraceComponent>(TEXT("MeleeTraceComponent"));

	// Controller 的生命周期由 Encounter 管理，不能让每个新形态自行生成一只 Controller。
	AutoPossessAI = EAutoPossessAI::Disabled;
}

void AGGYGOBossCharacter::OnDeathStarted(AActor* OwningActor)
{
	const TWeakObjectPtr<AGGYGOBossAIController> OriginalController = Cast<AGGYGOBossAIController>(GetController());
	Super::OnDeathStarted(OwningActor);
	if (AGGYGOBossAIController* BossController = OriginalController.Get(); BossController && BossController->GetPawn() == this)
	{
		BossController->StopCombatLogic(TEXT("Boss死亡"));
	}
}

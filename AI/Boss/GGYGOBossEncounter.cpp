/** @file GGYGOBossEncounter.cpp */
#include "AI/Boss/GGYGOBossEncounter.h"

#include "AI/Boss/GGYGOBossAIController.h"
#include "AI/Boss/GGYGOBossCharacter.h"
#include "AI/Boss/GGYGOBossDefinition.h"
#include "AI/Boss/GGYGOBossState.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Components/GGYGOHealthComponent.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Components/SceneComponent.h"
#include "Kismet/GameplayStatics.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossEncounter)

AGGYGOBossEncounter::AGGYGOBossEncounter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);
}

void AGGYGOBossEncounter::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority() && bSpawnOnBeginPlay)
	{
		SpawnBoss();
	}
}

bool AGGYGOBossEncounter::SpawnBoss()
{
	if (!HasAuthority() || BossState || BossController || BossAvatar)
	{
		return false;
	}

	if (!BossDefinition)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossEncounter [%s] 未配置 BossDefinition。"), *GetNameSafe(this));
		return false;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	FActorSpawnParameters StateSpawnParams;
	StateSpawnParams.Owner = this;
	AGGYGOBossState* NewState = World->SpawnActor<AGGYGOBossState>(
		AGGYGOBossState::StaticClass(), FTransform::Identity, StateSpawnParams);
	if (!NewState || !NewState->InitializeFromDefinition(BossDefinition))
	{
		if (NewState)
		{
			NewState->Destroy();
		}
		return false;
	}

	const UGGYGOPawnData* PawnData = NewState->GetInitialPawnData();
	UClass* PawnClass = PawnData ? PawnData->PawnClass.Get() : nullptr;
	if (!PawnClass || !PawnClass->IsChildOf(AGGYGOBossCharacter::StaticClass()))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossEncounter [%s] 的初始 PawnClass [%s] 必须继承 AGGYGOBossCharacter。"),
			*GetNameSafe(this), *GetNameSafe(PawnClass));
		NewState->Destroy();
		return false;
	}

	FActorSpawnParameters ControllerSpawnParams;
	ControllerSpawnParams.Owner = this;
	AGGYGOBossAIController* NewController = World->SpawnActor<AGGYGOBossAIController>(
		AGGYGOBossAIController::StaticClass(), GetActorTransform(), ControllerSpawnParams);
	if (!NewController)
	{
		NewState->Destroy();
		return false;
	}

	AGGYGOBossCharacter* NewAvatar = World->SpawnActorDeferred<AGGYGOBossCharacter>(
		PawnClass,
		GetActorTransform(),
		this,
		nullptr,
		ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
	if (!NewAvatar)
	{
		NewController->Destroy();
		NewState->Destroy();
		return false;
	}

	UGGYGOPawnExtensionComponent* PawnExtension = NewAvatar->GetPawnExtensionComponent();
	check(PawnExtension);
	PawnExtension->SetPawnData(PawnData);
	NewState->AttachAvatar(NewAvatar);
	UGameplayStatics::FinishSpawningActor(NewAvatar, GetActorTransform());
	NewController->Possess(NewAvatar);

	if (NewController->GetPawn() != NewAvatar || NewState->GetAvatarPawn() != NewAvatar)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossEncounter [%s] 装配后 Controller/State 未指向同一 Avatar。"), *GetNameSafe(this));
		NewAvatar->Destroy();
		NewController->Destroy();
		NewState->Destroy();
		return false;
	}

	BossState = NewState;
	BossController = NewController;
	BossAvatar = NewAvatar;

	const UGGYGOHealthComponent* HealthComponent = NewAvatar->GetHealthComponent();
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("BossEncounter: [%s] 已装配 State [%s]、Controller [%s]、Avatar [%s]；Form [%s]，Phase [%s]，Health [%.1f/%.1f]。"),
		*GetNameSafe(BossDefinition), *GetNameSafe(BossState),
		*GetNameSafe(BossController), *GetNameSafe(BossAvatar),
		*BossState->GetCurrentFormTag().ToString(), *BossState->GetCurrentPhaseTag().ToString(),
		HealthComponent ? HealthComponent->GetHealth() : 0.0f,
		HealthComponent ? HealthComponent->GetMaxHealth() : 0.0f);
	return true;
}

/** @file GGYGOBossEncounter.cpp */
#include "AI/Boss/GGYGOBossEncounter.h"

#include "AI/Boss/GGYGOBossAIController.h"
#include "AI/Boss/GGYGOBossCharacter.h"
#include "AI/Boss/GGYGOBossDefinition.h"
#include "AI/Boss/GGYGOBossState.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "BrainComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Components/GGYGOHealthComponent.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Components/SceneComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Templates/UnrealTemplate.h"

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

void AGGYGOBossEncounter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bEndingPlay = true;
	CleanupCreatedBoss();
	Super::EndPlay(EndPlayReason);
}

void AGGYGOBossEncounter::CleanupCreatedBoss()
{
	if (bCleaningUpBoss)
	{
		return;
	}
	TGuardValue<bool> CleanupGuard(bCleaningUpBoss, true);

	AGGYGOBossState* StateToDestroy = CreatedBossState.Get();
	AGGYGOBossAIController* ControllerToDestroy = CreatedBossController.Get();
	AGGYGOBossCharacter* AvatarToDestroy = CreatedBossAvatar.Get();
	BossState = nullptr;
	BossController = nullptr;
	BossAvatar = nullptr;
	CreatedBossState.Reset();
	CreatedBossController.Reset();
	CreatedBossAvatar.Reset();

	// Clear references before lifecycle callbacks: a reentrant cleanup sees no resources,
	// and SpawnBoss cannot start a replacement while these objects are being released.
	if (IsValid(ControllerToDestroy))
	{
		ControllerToDestroy->ClearActionSelection();
		if (UBrainComponent* Brain = ControllerToDestroy->GetBrainComponent(); IsValid(Brain))
		{
			Brain->StopLogic(TEXT("Boss Encounter ended"));
		}
	}
	if (IsValid(ControllerToDestroy) && ControllerToDestroy->GetPawn())
	{
		ControllerToDestroy->UnPossess();
	}
	if (IsValid(StateToDestroy))
	{
		if (APawn* CurrentAvatar = StateToDestroy->GetAvatarPawn())
		{
			// CombatantState owns ExpectedASC cleanup. The current Avatar may be external;
			// detaching it does not add it to our creation record or destruction scope.
			StateToDestroy->DetachAvatar(CurrentAvatar);
		}
	}

	if (IsValid(AvatarToDestroy))
	{
		AvatarToDestroy->Destroy();
	}
	if (IsValid(ControllerToDestroy))
	{
		ControllerToDestroy->Destroy();
	}
	if (IsValid(StateToDestroy))
	{
		StateToDestroy->Destroy();
	}
}

bool AGGYGOBossEncounter::SpawnBoss()
{
	if (!HasAuthority() || bSpawningBoss || bCleaningUpBoss || bEndingPlay || IsActorBeingDestroyed() ||
		BossState || BossController || BossAvatar || CreatedBossState.IsValid() ||
		CreatedBossController.IsValid() || CreatedBossAvatar.IsValid())
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
	TGuardValue<bool> SpawnGuard(bSpawningBoss, true);
	const UGGYGOBossDefinition* SpawnDefinition = BossDefinition;
	const auto FailSpawn = [this]()
	{
		CleanupCreatedBoss();
		return false;
	};
	const auto CanContinueSpawn = [this]()
	{
		return IsValid(this) && !bEndingPlay && !IsActorBeingDestroyed();
	};

	FActorSpawnParameters StateSpawnParams;
	StateSpawnParams.Owner = this;
	AGGYGOBossState* NewState = World->SpawnActor<AGGYGOBossState>(
		AGGYGOBossState::StaticClass(), FTransform::Identity, StateSpawnParams);
	CreatedBossState = NewState;
	if (!CanContinueSpawn() || !IsValid(NewState) || !NewState->InitializeFromDefinition(SpawnDefinition))
	{
		return FailSpawn();
	}
	if (!CanContinueSpawn() || !IsValid(NewState) || CreatedBossState.Get() != NewState)
	{
		return FailSpawn();
	}

	const UGGYGOPawnData* PawnData = NewState->GetInitialPawnData();
	UClass* PawnClass = PawnData ? PawnData->PawnClass.Get() : nullptr;
	if (!PawnClass || !PawnClass->IsChildOf(AGGYGOBossCharacter::StaticClass()))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossEncounter [%s] 的初始 PawnClass [%s] 必须继承 AGGYGOBossCharacter。"),
			*GetNameSafe(this), *GetNameSafe(PawnClass));
		return FailSpawn();
	}

	FActorSpawnParameters ControllerSpawnParams;
	ControllerSpawnParams.Owner = this;
	AGGYGOBossAIController* NewController = World->SpawnActor<AGGYGOBossAIController>(
		AGGYGOBossAIController::StaticClass(), GetActorTransform(), ControllerSpawnParams);
	CreatedBossController = NewController;
	if (!CanContinueSpawn() || !IsValid(NewController) || !IsValid(NewState) ||
		CreatedBossState.Get() != NewState)
	{
		return FailSpawn();
	}
	NewController->InitializeDecisionStream(EncounterSeed);

	AGGYGOBossCharacter* NewAvatar = World->SpawnActorDeferred<AGGYGOBossCharacter>(
		PawnClass,
		GetActorTransform(),
		this,
		nullptr,
		ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
	CreatedBossAvatar = NewAvatar;
	const auto IsAssemblyCurrent = [this, &CanContinueSpawn, NewState, NewController, NewAvatar]()
	{
		return CanContinueSpawn() && IsValid(NewState) && IsValid(NewController) && IsValid(NewAvatar) &&
			CreatedBossState.Get() == NewState && CreatedBossController.Get() == NewController &&
			CreatedBossAvatar.Get() == NewAvatar;
	};
	if (!IsAssemblyCurrent())
	{
		return FailSpawn();
	}

	UGGYGOPawnExtensionComponent* PawnExtension = NewAvatar->GetPawnExtensionComponent();
	if (!IsValid(PawnExtension))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossEncounter [%s] 的 Avatar [%s] 缺少有效 PawnExtension。"),
			*GetNameSafe(this), *GetNameSafe(NewAvatar));
		return FailSpawn();
	}
	PawnExtension->SetPawnData(PawnData);
	if (!IsAssemblyCurrent())
	{
		return FailSpawn();
	}
	NewState->AttachAvatar(NewAvatar);
	if (!IsAssemblyCurrent() || NewState->GetAvatarPawn() != NewAvatar)
	{
		return FailSpawn();
	}
	UGameplayStatics::FinishSpawningActor(NewAvatar, GetActorTransform());
	if (!IsAssemblyCurrent())
	{
		return FailSpawn();
	}
	// Possess may destroy the original assembly or definition through lifecycle callbacks.
	const FString EncounterName = GetNameSafe(this);
	const FString ControllerName = GetNameSafe(NewController);
	const FString AvatarName = GetNameSafe(NewAvatar);
	const FString DefinitionName = IsValid(SpawnDefinition) ? GetNameSafe(SpawnDefinition) : TEXT("<invalid>");
	const FString TreeName = IsValid(SpawnDefinition)
		? GetNameSafe(SpawnDefinition->BehaviorTree.Get()) : TEXT("<invalid>");
	FString PossessFailure;
	const EInitialBossPossessResult PossessResult = NewController->PossessInitialBoss(
		NewAvatar, NewState, SpawnDefinition, PossessFailure);
	if (PossessResult == EInitialBossPossessResult::Failed)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossEncounter [%s] 初始装配失败：Controller [%s]，Avatar [%s]，Definition [%s]，Tree [%s]。%s"),
			*EncounterName, *ControllerName, *AvatarName, *DefinitionName, *TreeName, *PossessFailure);
		return FailSpawn();
	}

	if (!IsAssemblyCurrent() || NewController->GetPawn() != NewAvatar || NewState->GetAvatarPawn() != NewAvatar
		|| NewState->GetBossDefinition() != SpawnDefinition || BossDefinition != SpawnDefinition)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossEncounter [%s] 装配后 Controller/State 未指向同一 Avatar。"), *GetNameSafe(this));
		return FailSpawn();
	}

	BossState = NewState;
	BossController = NewController;
	BossAvatar = NewAvatar;

	const UGGYGOHealthComponent* HealthComponent = NewAvatar->GetHealthComponent();
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("BossEncounter: [%s] 已装配 State [%s]、Controller [%s]、Avatar [%s]；Form [%s]，Phase [%s]，Health [%.1f/%.1f]。"),
		*GetNameSafe(SpawnDefinition), *GetNameSafe(BossState),
		*GetNameSafe(BossController), *GetNameSafe(BossAvatar),
		*BossState->GetCurrentFormTag().ToString(), *BossState->GetCurrentPhaseTag().ToString(),
		HealthComponent ? HealthComponent->GetHealth() : 0.0f,
		HealthComponent ? HealthComponent->GetMaxHealth() : 0.0f);
	return true;
}

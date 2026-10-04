#pragma once

#include "AI/Boss/GGYGOBossEncounter.h"
#include "AI/Boss/GGYGOBossCharacter.h"
#include "AI/Boss/GGYGOBossDefinition.h"
#include "BrainComponent.h"

#include "GGYGOBossEncounterLifecycleTestTypes.generated.h"

class AGGYGOBossState;
class AGGYGOBossAIController;
class UGGYGOAbilitySystemComponent;
class UGGYGOEncounterLifecycleObserver;

/** Only exposes production protected lifecycle/configuration; never writes creation records. */
UCLASS(Transient)
class AGGYGOEncounterLifecycleTestEncounter : public AGGYGOBossEncounter
{
	GENERATED_BODY()

public:
	void ConfigureForTest(const UGGYGOBossDefinition* Definition)
	{
		BossDefinition = Definition;
		bSpawnOnBeginPlay = false;
	}
	void CleanupForTest() { CleanupCreatedBoss(); }
	// The fixture never begins play: call the real override explicitly, not Destroy as a proxy.
	void EndPlayForTest() { EndPlay(EEndPlayReason::Destroyed); }
	void HandleAvatarInitializationForTest();

	bool bCleanupDuringAvatarInitialization = false;
	bool bInitializationCallbackRan = false;
	bool bInitializationSpawnRejected = false;
};

UCLASS(Transient)
class AGGYGOEncounterLifecycleTestPawn : public AGGYGOBossCharacter
{
	GENERATED_BODY()

public:
	virtual void UnPossessed() override;

	UPROPERTY()
	TObjectPtr<UGGYGOEncounterLifecycleObserver> Observer;

protected:
	virtual void PostInitializeComponents() override;
};

/** Observes actual StopLogic/UnPossessed/uninitialization/OnDestroyed calls in one timeline. */
UCLASS(Transient)
class UGGYGOEncounterLifecycleObserver : public UObject
{
	GENERATED_BODY()

public:
	void HandleStop();
	void HandleUnpossessed();
	void HandleUninitialized();
	UFUNCTION()
	void HandleDestroyed(AActor* Actor);

	UPROPERTY()
	TObjectPtr<AGGYGOEncounterLifecycleTestEncounter> Encounter;
	UPROPERTY()
	TObjectPtr<AGGYGOBossState> State;
	UPROPERTY()
	TObjectPtr<AGGYGOBossAIController> Controller;
	UPROPERTY()
	TObjectPtr<AGGYGOEncounterLifecycleTestPawn> CurrentAvatar;
	UPROPERTY()
	TObjectPtr<AGGYGOBossCharacter> CreatedAvatar;
	UPROPERTY()
	TObjectPtr<UGGYGOAbilitySystemComponent> ASC;

	TArray<FName> Events;
	int32 StopCount = 0;
	int32 UninitializedCount = 0;
	bool bReenterEndPlay = false;
	bool bReferencesEmptyAtStop = false;
	bool bStillBoundAtStop = false;
	bool bCreatedActorsAliveAtStop = false;
	bool bSpawnRejectedDuringCleanup = false;
	bool bDetachedBeforeDestroy = false;
};

UCLASS(Transient)
class UGGYGOEncounterLifecycleTestBrain : public UBrainComponent
{
	GENERATED_BODY()

public:
	virtual void StopLogic(const FString& Reason) override;
	UPROPERTY()
	TObjectPtr<UGGYGOEncounterLifecycleObserver> Observer;
};

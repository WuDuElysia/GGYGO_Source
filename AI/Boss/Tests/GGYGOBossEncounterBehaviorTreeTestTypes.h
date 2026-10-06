#pragma once

#include "AI/Boss/GGYGOBossEncounter.h"
#include "AI/Boss/GGYGOBossCharacter.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BrainComponent.h"

#include "GGYGOBossEncounterBehaviorTreeTestTypes.generated.h"

class AGGYGOBossAIController;
class AGGYGOBossState;
class UBehaviorTree;
class UGGYGOAbilitySystemComponent;
class UGGYGOEncounterBTProbe;
class UGGYGOEncounterBTLatentTask;

/** Exposes existing configuration/cleanup only; production creation records remain private. */
UCLASS(Transient)
class AGGYGOEncounterBTTestEncounter : public AGGYGOBossEncounter
{
	GENERATED_BODY()
public:
	void ConfigureForTest(const UGGYGOBossDefinition* Definition, UGGYGOEncounterBTProbe* InProbe);
	void CleanupForTest() { CleanupCreatedBoss(); }
	UGGYGOEncounterBTProbe* GetProbeForTest() const;
private:
	UPROPERTY()
	TObjectPtr<UGGYGOEncounterBTProbe> Probe;
};

UCLASS(Transient)
class AGGYGOEncounterBTTestPawn : public AGGYGOBossCharacter
{
	GENERATED_BODY()
public:
	virtual void UnPossessed() override;
	UGGYGOEncounterBTProbe* GetProbeForTest() const;
protected:
	virtual void PostInitializeComponents() override;
private:
	UPROPERTY()
	TObjectPtr<UGGYGOEncounterBTProbe> Probe;
};

/** Observed facts belong to this fixture; no Brain/ASC authority state is reproduced here. */
UCLASS(Transient)
class UGGYGOEncounterBTProbe : public UObject
{
	GENERATED_BODY()
public:
	void HandleWitnessMessage(UBrainComponent* Owner, const FAIMessage& Message);
	void CheckMessagesDuringAbort(UBehaviorTreeComponent& Owner);
	void HandleUnpossessed();
	void HandleUninitialized();
	void HandleInitialPawnChanged(APawn* NewPawn);
	UFUNCTION()
	void HandleDestroyed(AActor* Actor);

	UPROPERTY()
	TWeakObjectPtr<AGGYGOEncounterBTTestEncounter> Encounter;
	UPROPERTY()
	TWeakObjectPtr<AGGYGOBossState> State;
	UPROPERTY()
	TWeakObjectPtr<AGGYGOBossAIController> Controller;
	UPROPERTY()
	TWeakObjectPtr<AGGYGOEncounterBTTestPawn> Avatar;
	UPROPERTY()
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
	UPROPERTY()
	TWeakObjectPtr<UBehaviorTreeComponent> Brain;
	UPROPERTY()
	TWeakObjectPtr<UBehaviorTree> Tree;
	UPROPERTY()
	TWeakObjectPtr<UGGYGOEncounterBTLatentTask> RuntimeTask;

	FAIMessageObserverHandle MessageWitness;
	TArray<FName> Events;
	int32 ExecuteCount = 0;
	int32 TickCount = 0;
	int32 AbortCount = 0;
	int32 AbortTickCount = 0;
	int32 FinishLatentAbortCount = 0;
	int32 AbortedNotificationCount = 0;
	int32 InstanceCreatedCount = 0;
	int32 InstanceDestroyedCount = 0;
	int32 MemoryDestroyedCount = 0;
	int32 TaskMessageCount = 0;
	int32 WitnessMessageCount = 0;
	int32 UnpossessCount = 0;
	int32 DetachCount = 0;
	int32 AvatarDestroyedCount = 0;
	int32 ControllerDestroyedCount = 0;
	int32 StateDestroyedCount = 0;
	int32 InitialPossessCount = 0;
	bool bInitialAssemblyBound = false;
	bool bInitialBrainWithoutStartedTree = false;
	bool bBrainStoppedWithoutInstanceAtControllerDestroy = false;
	bool bRecording = true;
	bool bReferencesClearedAtAbort = false;
	bool bAssemblyBoundAtAbort = false;
	bool bCreatedActorsAliveAtAbort = false;
	bool bMessagesRemovedDuringLiveAbort = false;
	bool bPendingAbortAtUnpossess = false;
	bool bDetachedBeforeAvatarDestroy = false;
	bool bFinalStateObserved = false;
	bool bFinalStateStopped = false;
	bool bInstanceAndMemoryReleasedAtControllerDestroy = false;
};

/** Real instanced BT task. Its messages only observe; they never finish execution/abort. */
UCLASS(Transient)
class UGGYGOEncounterBTLatentTask : public UBTTaskNode
{
	GENERATED_BODY()
public:
	UGGYGOEncounterBTLatentTask(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& Owner, uint8* NodeMemory) override;
	virtual void OnInstanceCreated(UBehaviorTreeComponent& Owner) override;
	virtual void OnInstanceDestroyed(UBehaviorTreeComponent& Owner) override;
	virtual void CleanupMemory(UBehaviorTreeComponent& Owner, uint8* NodeMemory,
		EBTMemoryClear::Type CleanupType) const override;

	// Reflected so manager duplication and node instancing preserve the fixture configuration.
	UPROPERTY()
	bool bLatentAbort = true;
protected:
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& Owner, uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& Owner, uint8* NodeMemory, float DeltaSeconds) override;
	virtual void OnMessage(UBehaviorTreeComponent& Owner, uint8* NodeMemory,
		FName Message, int32 RequestID, bool bSuccess) override;
	virtual void OnTaskFinished(UBehaviorTreeComponent& Owner, uint8* NodeMemory,
		EBTNodeResult::Type Result) override;
private:
	UPROPERTY()
	TWeakObjectPtr<UGGYGOEncounterBTProbe> Probe;
	bool bAwaitingAbort = false;
	int32 AbortTicksRemaining = 0;
};

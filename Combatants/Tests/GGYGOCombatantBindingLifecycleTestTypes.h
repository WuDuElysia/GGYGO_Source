#pragma once

#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Combatants/GGYGOCombatantState.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Pawn.h"

#include "GGYGOCombatantBindingLifecycleTestTypes.generated.h"

/** Concrete persistent host used by the binding lifecycle automation tests. */
UCLASS(Transient)
class AGGYGOCombatantBindingTestState : public AGGYGOCombatantState
{
	GENERATED_BODY()

public:
	AGGYGOCombatantBindingTestState(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** The synthetic test World never begins play, so invoke the real EndPlay override explicitly. */
	void InvokeEndPlayForTest() { EndPlay(EEndPlayReason::Destroyed); }
};

/** Minimal Pawn carrying the production PawnExtension coordinator. */
UCLASS(Transient)
class AGGYGOCombatantBindingTestPawn : public APawn
{
	GENERATED_BODY()

public:
	AGGYGOCombatantBindingTestPawn(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	UGGYGOPawnExtensionComponent* GetPawnExtensionForTest() const { return PawnExtension; }

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> TestRoot;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UGGYGOPawnExtensionComponent> PawnExtension;
};

/** Counts the native uninitialization notification without changing production visibility. */
UCLASS(Transient)
class UGGYGOCombatantBindingTestObserver : public UObject
{
	GENERATED_BODY()

public:
	void HandleUninitialized() { ++UninitializedCount; }
	int32 GetUninitializedCount() const { return UninitializedCount; }

private:
	int32 UninitializedCount = 0;
};

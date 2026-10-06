#pragma once

#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Character/Components/GGYGOHealthComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Pawn.h"

#include "GGYGOCombatantDeathProjectionTestTypes.generated.h"

UCLASS(Transient)
class AGGYGOCombatantDeathProjectionTestPawn : public APawn
{
	GENERATED_BODY()

public:
	AGGYGOCombatantDeathProjectionTestPawn(
		const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	UGGYGOAbilitySystemComponent* GetASCForTest() const { return AbilitySystemComponent; }
	UGGYGOHealthSet* GetHealthSetForTest() const { return HealthSet; }
	UGGYGOHealthComponent* GetHealthForTest() const { return HealthComponent; }
	UGGYGOPawnExtensionComponent* GetExtensionForTest() const { return PawnExtension; }

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> TestRoot;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UGGYGOAbilitySystemComponent> AbilitySystemComponent;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UGGYGOHealthSet> HealthSet;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UGGYGOHealthComponent> HealthComponent;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UGGYGOPawnExtensionComponent> PawnExtension;
};

UCLASS(Transient)
class UGGYGOCombatantDeathProjectionTestObserver : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void HandleDeathStarted(AActor* OwningActor) { (void)OwningActor; ++DeathStartedCount; }

	UFUNCTION()
	void HandleDeathFinished(AActor* OwningActor) { (void)OwningActor; ++DeathFinishedCount; }

	int32 GetDeathStartedCount() const { return DeathStartedCount; }
	int32 GetDeathFinishedCount() const { return DeathFinishedCount; }

private:
	int32 DeathStartedCount = 0;
	int32 DeathFinishedCount = 0;
};

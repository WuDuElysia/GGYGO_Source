#pragma once

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "Character/GGYGOCharacterBase.h"
#include "GameplayEffect.h"

#include "GGYGOSquadSwitchTestTypes.generated.h"

class UGGYGOInputComponent;
class UGGYGOInputTestHeroComponent;

/** Real Character/Extension/Health; only its fixture input component and observations differ. */
UCLASS(Transient)
class AGGYGOSquadSwitchTestPawn : public AGGYGOCharacterBase
{
	GENERATED_BODY()

public:
	AGGYGOSquadSwitchTestPawn(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	UGGYGOInputTestHeroComponent* GetHeroForTest() const { return Hero; }
	UGGYGOInputComponent* GetInputComponentForTest() const;
	void ResetControlObservationsForTest();
	int32 PossessCalls = 0;
	int32 UnpossessCalls = 0;
	int32 ActiveCharacterNotices = 0;
	TWeakObjectPtr<AGGYGOCharacterBase> LastActiveCharacter;
	UFUNCTION()
	void ObserveActiveCharacterForTest(AGGYGOCharacterBase* NewCharacter);

protected:
	virtual UInputComponent* CreatePlayerInputComponent() override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void UnPossessed() override;

private:
	UPROPERTY()
	TObjectPtr<UGGYGOInputTestHeroComponent> Hero;
};

/** Configured cooldown uses a real duration GE and granted cooldown tag. */
UCLASS(Transient)
class UGGYGOSquadSwitchTestCooldown : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGGYGOSquadSwitchTestCooldown(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

/** Holds a real authoritative GAS activation; observations never supply lifecycle proof. */
UCLASS(Transient)
class UGGYGOSquadSwitchTestAbility : public UGGYGOGameplayAbility
{
	GENERATED_BODY()

public:
	UGGYGOSquadSwitchTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	/** Invoke the inherited project setter from the legal derived scope; no native bypass. */
	void SetCanBeCanceledForTest(bool bCanBeCanceled) { SetCanBeCanceled(bCanBeCanceled); }
	void ArmCleanupForTest(FSimpleDelegate Hook) { CleanupHook = MoveTemp(Hook); }
	void DisarmCleanupForTest() { CleanupHook.Unbind(); }
	int32 ActivationCalls = 0;
	int32 CleanupCalls = 0;
	FGGYGOAbilityActivationHandle ActivatedOriginal;
	FGGYGOAbilityActivationHandle CleanedOriginal;

protected:
	virtual void ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context) override;

private:
	FSimpleDelegate CleanupHook;
};

/** The fixture CDO explicitly selects the same policy that a character Blueprint configures. */
UCLASS(Transient)
class UGGYGOSquadSwitchContinueTestAbility : public UGGYGOSquadSwitchTestAbility
{
	GENERATED_BODY()
public:
	UGGYGOSquadSwitchContinueTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

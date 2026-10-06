#pragma once

#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "GameFramework/Actor.h"

#include "GGYGOHealthMessageTestTypes.generated.h"

/** Test-only actor that exposes a real project ASC through GAS's source interface. */
UCLASS(Transient)
class AGGYGOHealthMessageTestActor : public AActor, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AGGYGOHealthMessageTestActor();

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	UGGYGOAbilitySystemComponent* GetProjectAbilitySystemComponent() const { return AbilitySystemComponent; }
	void SetTestNetRole(ENetRole InRole) { SetRole(InRole); AbilitySystemComponent->CacheIsNetSimulated(); }

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UGGYGOAbilitySystemComponent> AbilitySystemComponent;
};

/** Exposes the existing protected RepNotify entry points without changing production interfaces. */
UCLASS(Transient)
class UGGYGOHealthMessageRepNotifyTestSet : public UGGYGOHealthSet
{
	GENERATED_BODY()

public:
	virtual UWorld* GetWorld() const override;
	void SetMessageWorldOverride(UWorld* World);
	void ClearMessageWorldOverride();
	void BeginTestNetReceive() { PreNetReceive(); }
	void EndTestNetReceive() { PostNetReceive(); }

	void SimulateReplicatedMaxHealth(float NewValue)
	{
		const FGameplayAttributeData OldValue = *GetMaxHealthAttribute().GetGameplayAttributeDataChecked(this);
		*GetMaxHealthAttribute().GetGameplayAttributeDataChecked(this) = FGameplayAttributeData(NewValue);
		OnRep_MaxHealth(OldValue);
	}

	void SimulateReplicatedMaxPoise(float NewValue)
	{
		const FGameplayAttributeData OldValue = *GetMaxPoiseAttribute().GetGameplayAttributeDataChecked(this);
		*GetMaxPoiseAttribute().GetGameplayAttributeDataChecked(this) = FGameplayAttributeData(NewValue);
		OnRep_MaxPoise(OldValue);
	}

	void SimulateReplicatedHealth(float NewValue)
	{
		FGameplayAttributeData NewAttributeValue(NewValue);
		SimulateReplicatedHealth(NewAttributeValue);
	}

	void SimulateReplicatedHealth(const FGameplayAttributeData& NewValue)
	{
		const FGameplayAttributeData OldValue = *GetHealthAttribute().GetGameplayAttributeDataChecked(this);
		*GetHealthAttribute().GetGameplayAttributeDataChecked(this) = NewValue;
		OnRep_Health(OldValue);
	}

	void SimulateReplicatedPoise(float NewValue)
	{
		FGameplayAttributeData NewAttributeValue(NewValue);
		SimulateReplicatedPoise(NewAttributeValue);
	}

	void SimulateReplicatedPoise(const FGameplayAttributeData& NewValue)
	{
		const FGameplayAttributeData OldValue = *GetPoiseAttribute().GetGameplayAttributeDataChecked(this);
		*GetPoiseAttribute().GetGameplayAttributeDataChecked(this) = NewValue;
		OnRep_Poise(OldValue);
	}

private:
	bool bOverrideMessageWorld = false;
	TWeakObjectPtr<UWorld> MessageWorldOverride;
};

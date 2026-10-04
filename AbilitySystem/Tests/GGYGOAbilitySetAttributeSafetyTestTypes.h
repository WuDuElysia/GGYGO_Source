#pragma once

#include "AbilitySystem/GGYGOAbilitySet.h"
#include "Combatants/GGYGOCombatantState.h"

#include "GGYGOAbilitySetAttributeSafetyTestTypes.generated.h"

/** Only configures the test asset's own protected attribute entries. */
UCLASS(Transient)
class UGGYGOAbilitySetAttributeSafetyTestAsset : public UGGYGOAbilitySet
{
	GENERATED_BODY()

public:
	UGGYGOAbilitySetAttributeSafetyTestAsset(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get())
		: Super(ObjectInitializer)
	{
	}

	void SetAttributeClassesForTest(const TArray<TSubclassOf<UAttributeSet>>& SetClasses)
	{
		GrantedAttributes.Reset(SetClasses.Num());
		for (const TSubclassOf<UAttributeSet>& SetClass : SetClasses)
		{
			GrantedAttributes.AddDefaulted_GetRef().AttributeSet = SetClass;
		}
	}
};

/** Observes inherited grant records without exposing mutable storage or adding state. */
struct FGGYGOAbilitySetAttributeSafetyHandlesView : public FGGYGOAbilitySet_GrantedHandles
{
	int32 GetAttributeSetCountForTest() const { return GrantedAttributeSets.Num(); }

	const UAttributeSet* GetAttributeSetForTest(int32 Index) const
	{
		return GrantedAttributeSets.IsValidIndex(Index) ? GrantedAttributeSets[Index].Get() : nullptr;
	}
};

/** Concrete parent and its children all store the same declared GAS field. */
UCLASS(Transient)
class UGGYGOAbilitySetAttributeSafetySharedSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FGameplayAttributeData SharedValue;
};

UCLASS(Transient)
class UGGYGOAbilitySetAttributeSafetySharedSiblingA : public UGGYGOAbilitySetAttributeSafetySharedSet
{
	GENERATED_BODY()
};

UCLASS(Transient)
class UGGYGOAbilitySetAttributeSafetySharedSiblingB : public UGGYGOAbilitySetAttributeSafetySharedSet
{
	GENERATED_BODY()
};

/** A common ancestor with no declared GAS storage. */
UCLASS(Transient)
class UGGYGOAbilitySetAttributeSafetyEmptyBase : public UAttributeSet
{
	GENERATED_BODY()
};

UCLASS(Transient)
class UGGYGOAbilitySetAttributeSafetyIndependentA : public UGGYGOAbilitySetAttributeSafetyEmptyBase
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FGameplayAttributeData IndependentAValue;
};

UCLASS(Transient)
class UGGYGOAbilitySetAttributeSafetyIndependentB : public UGGYGOAbilitySetAttributeSafetyEmptyBase
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FGameplayAttributeData IndependentBValue;
};

/** Class metadata only: this type must never be instantiated by the fixture. */
UCLASS(Abstract, Transient)
class UGGYGOAbilitySetAttributeSafetyAbstractSet : public UAttributeSet
{
	GENERATED_BODY()
};

/** Uses the production host's default ASC, HealthSet and CombatSet unchanged. */
UCLASS(Transient)
class AGGYGOAbilitySetAttributeSafetyTestHost : public AGGYGOCombatantState
{
	GENERATED_BODY()

public:
	AGGYGOAbilitySetAttributeSafetyTestHost(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get())
		: Super(ObjectInitializer)
	{
	}
};

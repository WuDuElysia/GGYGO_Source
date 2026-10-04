#pragma once

#include "AI/Boss/GGYGOBossState.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Teams/GGYGOCharacterSlot.h"

#include "GGYGOCombatantConfigReplicationTestTypes.generated.h"

class UGGYGOAbilityTagRelationshipMapping;
class UGGYGOBossDefinition;
class UGGYGOPawnData;

UCLASS(Transient)
class UGGYGOCombatantConfigTestASC : public UGGYGOAbilitySystemComponent
{
	GENERATED_BODY()

public:
	UGGYGOAbilityTagRelationshipMapping* GetTagRelationshipMappingForTest() const
	{
		return TagRelationshipMapping;
	}
};

UCLASS(Transient)
class AGGYGOCombatantConfigTestSlot : public AGGYGOCharacterSlot
{
	GENERATED_BODY()

public:
	AGGYGOCombatantConfigTestSlot(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	void SimulatePawnDataReplication(const UGGYGOPawnData* InPawnData);
	UGGYGOCombatantConfigTestASC* GetTestASC() const;
};

UCLASS(Transient)
class AGGYGOCombatantConfigTestBossState : public AGGYGOBossState
{
	GENERATED_BODY()

public:
	AGGYGOCombatantConfigTestBossState(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	void SimulateBossDefinitionReplication(const UGGYGOBossDefinition* InDefinition);
	UGGYGOCombatantConfigTestASC* GetTestASC() const;
};

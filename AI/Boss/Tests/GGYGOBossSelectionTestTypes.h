#pragma once

#include "AbilitySystem/Abilities/GGYGOCombatActionAbility.h"
#include "AI/Boss/GGYGOBossActionSet.h"
#include "AI/Boss/GGYGOBossAIController.h"
#include "System/GGYGOGameplayTags.h"

#include "GGYGOBossSelectionTestTypes.generated.h"

/** 测试用瞬态能力；每个 CDO 使用已有原生 BossAction Tag。 */
UCLASS(Transient)
class UGGYGOBossSelectionMeleeAbility : public UGGYGOCombatActionAbility
{
	GENERATED_BODY()

public:
	UGGYGOBossSelectionMeleeAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get())
		: Super(ObjectInitializer)
	{
		ActionTag = GGYGOGameplayTags::BossAction_Attack_Melee;
	}
};

UCLASS(Transient)
class UGGYGOBossSelectionMeleeAlternateAbility : public UGGYGOCombatActionAbility
{
	GENERATED_BODY()

public:
	UGGYGOBossSelectionMeleeAlternateAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get())
		: Super(ObjectInitializer)
	{
		ActionTag = GGYGOGameplayTags::BossAction_Attack_Melee;
	}
};

UCLASS(Transient)
class UGGYGOBossSelectionIceOneAbility : public UGGYGOCombatActionAbility
{
	GENERATED_BODY()

public:
	UGGYGOBossSelectionIceOneAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get())
		: Super(ObjectInitializer)
	{
		ActionTag = GGYGOGameplayTags::BossAction_Attack_Melee_Ice01;
	}
};

UCLASS(Transient)
class UGGYGOBossSelectionIceTwoAbility : public UGGYGOCombatActionAbility
{
	GENERATED_BODY()

public:
	UGGYGOBossSelectionIceTwoAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get())
		: Super(ObjectInitializer)
	{
		ActionTag = GGYGOGameplayTags::BossAction_Attack_Melee_Ice02;
	}
};

UCLASS(Abstract, Transient)
class UGGYGOBossSelectionAbstractAbility : public UGGYGOCombatActionAbility
{
	GENERATED_BODY()

public:
	UGGYGOBossSelectionAbstractAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get())
		: Super(ObjectInitializer)
	{
		ActionTag = GGYGOGameplayTags::BossAction_Attack_Melee;
	}
};

/** Exposes only protected selection state/lifecycle hooks to native automation tests. */
UCLASS(Transient)
class AGGYGOBossSelectionTestController : public AGGYGOBossAIController
{
	GENERATED_BODY()

public:
	void SetRuntimeWeightForTest(FGameplayTag ActionTag, float Weight)
	{
		RuntimeActionWeights.Add(ActionTag, Weight);
	}

	void SetWeightSourceForTest(const UGGYGOBossActionSet* ActionSet)
	{
		WeightSource = ActionSet;
	}

	bool TryGetRuntimeWeightForTest(FGameplayTag ActionTag, float& OutWeight) const
	{
		const float* Weight = RuntimeActionWeights.Find(ActionTag);
		if (!Weight)
		{
			return false;
		}
		OutWeight = *Weight;
		return true;
	}

	void InvokeOnUnPossessForTest()
	{
		OnUnPossess();
	}
};

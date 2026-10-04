#include "Combatants/Tests/GGYGOCombatantConfigReplicationTestTypes.h"

#include "AI/Boss/GGYGOBossDefinition.h"
#include "AbilitySystem/GGYGOAbilityTagRelationshipMapping.h"
#include "AbilitySystem/Groups/GGYGOAbilityGroupConfig.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCombatantConfigReplicationTestTypes)

AGGYGOCombatantConfigTestSlot::AGGYGOCombatantConfigTestSlot(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UGGYGOCombatantConfigTestASC>(
		FName(TEXT("AbilitySystemComponent"))))
{
}

void AGGYGOCombatantConfigTestSlot::SimulatePawnDataReplication(const UGGYGOPawnData* InPawnData)
{
	PawnData = InPawnData;
	OnRep_PawnData();
}

UGGYGOCombatantConfigTestASC* AGGYGOCombatantConfigTestSlot::GetTestASC() const
{
	return CastChecked<UGGYGOCombatantConfigTestASC>(GetGGYGOAbilitySystemComponent());
}

AGGYGOCombatantConfigTestBossState::AGGYGOCombatantConfigTestBossState(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UGGYGOCombatantConfigTestASC>(
		FName(TEXT("AbilitySystemComponent"))))
{
}

void AGGYGOCombatantConfigTestBossState::SimulateBossDefinitionReplication(
	const UGGYGOBossDefinition* InDefinition)
{
	BossDefinition = InDefinition;
	OnRep_BossDefinition();
}

UGGYGOCombatantConfigTestASC* AGGYGOCombatantConfigTestBossState::GetTestASC() const
{
	return CastChecked<UGGYGOCombatantConfigTestASC>(GetGGYGOAbilitySystemComponent());
}

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	struct FCombatantConfigTestWorld
	{
		UEngine* Engine = GEngine;
		UWorld* World = nullptr;

		FCombatantConfigTestWorld()
		{
			UWorld::InitializationValues Init;
			Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(false)
				.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Init);
			if (World && Engine)
			{
				Engine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			}
		}

		~FCombatantConfigTestWorld()
		{
			if (World)
			{
				World->DestroyWorld(false);
				if (Engine)
				{
					Engine->DestroyWorldContext(World);
				}
				if (UPackage* Package = World->GetPackage())
				{
					Package->SetDirtyFlag(false);
				}
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCombatantConfigReplicationTest,
	"GGYGO.Combatants.Config.ClientReplicationCallbacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCombatantConfigReplicationTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FCombatantConfigTestWorld Fixture;
	if (!TestNotNull(TEXT("test world"), Fixture.World)) { return false; }

	TStrongObjectPtr<UGGYGOAbilityGroupConfig> GroupConfig(
		NewObject<UGGYGOAbilityGroupConfig>(GetTransientPackage()));
	TStrongObjectPtr<UGGYGOAbilityTagRelationshipMapping> TagMapping(
		NewObject<UGGYGOAbilityTagRelationshipMapping>(GetTransientPackage()));
	TStrongObjectPtr<UGGYGOPawnData> PawnData(NewObject<UGGYGOPawnData>(GetTransientPackage()));
	PawnData->AbilityGroupConfig = GroupConfig.Get();
	PawnData->TagRelationshipMapping = TagMapping.Get();

	AGGYGOCombatantConfigTestSlot* Slot = Fixture.World->SpawnActor<AGGYGOCombatantConfigTestSlot>();
	if (!TestNotNull(TEXT("test Slot"), Slot)) { return false; }
	UGGYGOCombatantConfigTestASC* SlotASC = Slot->GetTestASC();
	const int32 SlotAbilityCount = SlotASC->GetActivatableAbilities().Num();
	Slot->SimulatePawnDataReplication(PawnData.Get());
	Slot->SimulatePawnDataReplication(PawnData.Get());
	TestEqual(TEXT("Slot OnRep installs the group config"), SlotASC->GetAbilityGroupConfig(),
		static_cast<const UGGYGOAbilityGroupConfig*>(GroupConfig.Get()));
	TestEqual(TEXT("Slot OnRep installs the tag relationship mapping"),
		SlotASC->GetTagRelationshipMappingForTest(), TagMapping.Get());
	TestEqual(TEXT("repeated Slot OnRep does not grant abilities"),
		SlotASC->GetActivatableAbilities().Num(), SlotAbilityCount);

	TStrongObjectPtr<UGGYGOBossDefinition> BossDefinition(
		NewObject<UGGYGOBossDefinition>(GetTransientPackage()));
	BossDefinition->InitialFormTag = GGYGOGameplayTags::State_Boss_Form_Default;
	FGGYGOBossFormDefinition& InitialForm = BossDefinition->Forms.AddDefaulted_GetRef();
	InitialForm.FormTag = BossDefinition->InitialFormTag;
	InitialForm.AvatarPawnData = PawnData.Get();

	AGGYGOCombatantConfigTestBossState* BossState =
		Fixture.World->SpawnActor<AGGYGOCombatantConfigTestBossState>();
	if (!TestNotNull(TEXT("test BossState"), BossState)) { return false; }
	UGGYGOCombatantConfigTestASC* BossASC = BossState->GetTestASC();
	const int32 BossAbilityCount = BossASC->GetActivatableAbilities().Num();
	BossState->SimulateBossDefinitionReplication(BossDefinition.Get());
	BossState->SimulateBossDefinitionReplication(BossDefinition.Get());
	TestEqual(TEXT("BossDefinition OnRep installs the initial form group config"),
		BossASC->GetAbilityGroupConfig(), static_cast<const UGGYGOAbilityGroupConfig*>(GroupConfig.Get()));
	TestEqual(TEXT("BossDefinition OnRep installs the initial form tag relationship mapping"),
		BossASC->GetTagRelationshipMappingForTest(), TagMapping.Get());
	TestEqual(TEXT("repeated BossDefinition OnRep does not grant abilities"),
		BossASC->GetActivatableAbilities().Num(), BossAbilityCount);

	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS

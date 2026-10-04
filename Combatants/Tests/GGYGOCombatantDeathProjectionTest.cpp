#include "Combatants/Tests/GGYGOCombatantDeathProjectionTestTypes.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCombatantDeathProjectionTestTypes)

AGGYGOCombatantDeathProjectionTestPawn::AGGYGOCombatantDeathProjectionTestPawn(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	TestRoot = CreateDefaultSubobject<USceneComponent>(TEXT("TestRoot"));
	SetRootComponent(TestRoot);
	AbilitySystemComponent = CreateDefaultSubobject<UGGYGOAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	HealthSet = CreateDefaultSubobject<UGGYGOHealthSet>(TEXT("HealthSet"));
	HealthComponent = CreateDefaultSubobject<UGGYGOHealthComponent>(TEXT("HealthComponent"));
}

void AGGYGOCombatantDeathProjectionTestPawn::InitializeAbilitySystemForTest(AActor* AvatarActor)
{
	if (!AbilitySystemComponent->GetSet<UGGYGOHealthSet>())
	{
		AbilitySystemComponent->AddAttributeSetSubobject(HealthSet.Get());
	}
	AbilitySystemComponent->InitAbilityActorInfo(this, AvatarActor);
}

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	struct FCombatantDeathTestWorld
	{
		UEngine* Engine = GEngine;
		UWorld* World = nullptr;

		FCombatantDeathTestWorld()
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

		~FCombatantDeathTestWorld()
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

	void ObserveDeath(UGGYGOHealthComponent* Health,
		UGGYGOCombatantDeathProjectionTestObserver* Observer)
	{
		Health->OnDeathStarted.AddDynamic(
			Observer, &UGGYGOCombatantDeathProjectionTestObserver::HandleDeathStarted);
		Health->OnDeathFinished.AddDynamic(
			Observer, &UGGYGOCombatantDeathProjectionTestObserver::HandleDeathFinished);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCombatantDeathProjectionStateTest,
	"GGYGO.Combatants.DeathProjection.MonotonicAndPersistent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCombatantDeathProjectionStateTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FCombatantDeathTestWorld Fixture;
	if (!TestNotNull(TEXT("test world"), Fixture.World)) { return false; }

	AGGYGOCombatantDeathProjectionTestPawn* Pawn =
		Fixture.World->SpawnActor<AGGYGOCombatantDeathProjectionTestPawn>();
	if (!TestNotNull(TEXT("test Pawn"), Pawn)) { return false; }
	UGGYGOAbilitySystemComponent* ASC = Pawn->GetASCForTest();
	UGGYGOHealthComponent* Health = Pawn->GetHealthForTest();
	Pawn->InitializeAbilitySystemForTest(Pawn);
	if (!TestNotNull(TEXT("fixture ASC has production HealthSet"), ASC->GetSet<UGGYGOHealthSet>()))
	{
		return false;
	}
	Health->InitializeWithAbilitySystem(ASC);

	TStrongObjectPtr<UGGYGOCombatantDeathProjectionTestObserver> Observer(
		NewObject<UGGYGOCombatantDeathProjectionTestObserver>(GetTransientPackage()));
	ObserveDeath(Health, Observer.Get());

	Health->StartDeath();
	Health->StartDeath();
	TestEqual(TEXT("DeathStarted projects Dying once"), ASC->GetTagCount(GGYGOGameplayTags::State_Dying), 1);
	TestEqual(TEXT("DeathStarted does not project Dead"), ASC->GetTagCount(GGYGOGameplayTags::State_Dead), 0);
	TestEqual(TEXT("repeated StartDeath does not replay the event"), Observer->GetDeathStartedCount(), 1);

	Health->FinishDeath();
	Health->FinishDeath();
	TestEqual(TEXT("DeathFinished keeps Dying"), ASC->GetTagCount(GGYGOGameplayTags::State_Dying), 1);
	TestEqual(TEXT("DeathFinished projects Dead once"), ASC->GetTagCount(GGYGOGameplayTags::State_Dead), 1);
	TestEqual(TEXT("repeated FinishDeath does not replay the event"), Observer->GetDeathFinishedCount(), 1);

	Health->UninitializeFromAbilitySystem();
	TestEqual(TEXT("unbind preserves Dying on persistent ASC"), ASC->GetTagCount(GGYGOGameplayTags::State_Dying), 1);
	TestEqual(TEXT("unbind preserves Dead on persistent ASC"), ASC->GetTagCount(GGYGOGameplayTags::State_Dead), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCombatantDeathProjectionBindingTest,
	"GGYGO.Combatants.DeathProjection.LateBindingAndAvatarIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCombatantDeathProjectionBindingTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FCombatantDeathTestWorld Fixture;
	if (!TestNotNull(TEXT("test world"), Fixture.World)) { return false; }

	AGGYGOCombatantDeathProjectionTestPawn* LatePawn =
		Fixture.World->SpawnActor<AGGYGOCombatantDeathProjectionTestPawn>();
	if (!TestNotNull(TEXT("late-binding Pawn"), LatePawn)) { return false; }
	UGGYGOAbilitySystemComponent* LateASC = LatePawn->GetASCForTest();
	UGGYGOHealthComponent* LateHealth = LatePawn->GetHealthForTest();
	LatePawn->InitializeAbilitySystemForTest(LatePawn);
	if (!TestNotNull(TEXT("late fixture ASC has production HealthSet"),
		LateASC->GetSet<UGGYGOHealthSet>()))
	{
		return false;
	}
	TStrongObjectPtr<UGGYGOCombatantDeathProjectionTestObserver> LateObserver(
		NewObject<UGGYGOCombatantDeathProjectionTestObserver>(GetTransientPackage()));
	ObserveDeath(LateHealth, LateObserver.Get());
	LateHealth->StartDeath();
	LateHealth->FinishDeath();
	TestEqual(TEXT("unbound death emitted one start event"), LateObserver->GetDeathStartedCount(), 1);
	TestEqual(TEXT("unbound death emitted one finish event"), LateObserver->GetDeathFinishedCount(), 1);
	LateHealth->InitializeWithAbilitySystem(LateASC);
	TestEqual(TEXT("late binding replays Dying state"), LateASC->GetTagCount(GGYGOGameplayTags::State_Dying), 1);
	TestEqual(TEXT("late binding replays Dead state"), LateASC->GetTagCount(GGYGOGameplayTags::State_Dead), 1);
	TestEqual(TEXT("late binding does not replay start event"), LateObserver->GetDeathStartedCount(), 1);
	TestEqual(TEXT("late binding does not replay finish event"), LateObserver->GetDeathFinishedCount(), 1);

	AGGYGOCombatantDeathProjectionTestPawn* MismatchedOwner =
		Fixture.World->SpawnActor<AGGYGOCombatantDeathProjectionTestPawn>();
	AGGYGOCombatantDeathProjectionTestPawn* OtherAvatar =
		Fixture.World->SpawnActor<AGGYGOCombatantDeathProjectionTestPawn>();
	if (!TestNotNull(TEXT("mismatched owner"), MismatchedOwner)
		|| !TestNotNull(TEXT("other Avatar"), OtherAvatar))
	{
		return false;
	}
	UGGYGOAbilitySystemComponent* MismatchedASC = MismatchedOwner->GetASCForTest();
	UGGYGOHealthComponent* MismatchedHealth = MismatchedOwner->GetHealthForTest();
	MismatchedOwner->InitializeAbilitySystemForTest(OtherAvatar);
	if (!TestNotNull(TEXT("mismatched fixture ASC has production HealthSet"),
		MismatchedASC->GetSet<UGGYGOHealthSet>()))
	{
		return false;
	}
	MismatchedHealth->InitializeWithAbilitySystem(MismatchedASC);
	MismatchedHealth->StartDeath();
	MismatchedHealth->FinishDeath();
	TestEqual(TEXT("non-owner Avatar blocks Dying projection"),
		MismatchedASC->GetTagCount(GGYGOGameplayTags::State_Dying), 0);
	TestEqual(TEXT("non-owner Avatar blocks Dead projection"),
		MismatchedASC->GetTagCount(GGYGOGameplayTags::State_Dead), 0);

	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS

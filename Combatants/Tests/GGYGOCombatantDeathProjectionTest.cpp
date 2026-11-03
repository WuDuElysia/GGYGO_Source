#include "Combatants/Tests/GGYGOCombatantDeathProjectionTestTypes.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
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
	PawnExtension = CreateDefaultSubobject<UGGYGOPawnExtensionComponent>(TEXT("PawnExtension"));
}

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	// 清理输入只来自真实的原生 ActorInfo 写入与 PawnExtension 就绪资源。
	struct FDeathProjectionBinding
	{
		TWeakObjectPtr<AGGYGOCombatantDeathProjectionTestPawn> Owner;
		TWeakObjectPtr<AGGYGOCombatantDeathProjectionTestPawn> Avatar;
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
		TWeakObjectPtr<UGGYGOPawnExtensionComponent> Extension;
		FGGYGOPawnASCResourceHandle Resource;
	};

	bool AreOriginalEndpointsCurrent(const FDeathProjectionBinding& Binding)
	{
		return Binding.Owner.IsValid() && !Binding.Owner->IsActorBeingDestroyed()
			&& Binding.Avatar.IsValid() && !Binding.Avatar->IsActorBeingDestroyed()
			&& Binding.ASC.IsValid() && Binding.ASC->IsRegistered()
			&& Binding.Owner->GetASCForTest() == Binding.ASC.Get()
			&& Binding.ASC->GetOwner() == Binding.Owner.Get()
			&& Binding.Extension.IsValid() && Binding.Extension->IsRegistered()
			&& Binding.Avatar->GetExtensionForTest() == Binding.Extension.Get()
			&& Binding.Extension->GetOwner() == Binding.Avatar.Get()
			&& Binding.Owner->GetWorld() == Binding.Avatar->GetWorld();
	}

	struct FCombatantDeathTestWorld
	{
		FAutomationTestBase& Test;
		UEngine* Engine = GEngine;
		UWorld* World = nullptr;
		TArray<FDeathProjectionBinding> Bindings;

		explicit FCombatantDeathTestWorld(FAutomationTestBase& InTest) : Test(InTest)
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

		bool BindAvatar(AGGYGOCombatantDeathProjectionTestPawn* Owner,
			AGGYGOCombatantDeathProjectionTestPawn* Avatar, FDeathProjectionBinding& OutBinding)
		{
			OutBinding = {};
			if (!Test.TestNotNull(TEXT("binding original Owner"), Owner)
				|| !Test.TestNotNull(TEXT("binding original Avatar"), Avatar)) { return false; }
			FDeathProjectionBinding Binding;
			Binding.Owner = Owner;
			Binding.Avatar = Avatar;
			Binding.ASC = Owner->GetASCForTest();
			Binding.Extension = Avatar->GetExtensionForTest();
			const auto IsOriginalCallerCurrent = [Binding]() { return AreOriginalEndpointsCurrent(Binding); };
			if (!Test.TestTrue(TEXT("binding registered original production endpoints"), IsOriginalCallerCurrent())
				|| !Test.TestTrue(TEXT("binding isolated PreBegin World"), World && !World->HasBegunPlay())
				|| !Test.TestNotNull(TEXT("binding original HealthSet"), Owner->GetHealthSetForTest())) { return false; }
			UGGYGOAbilitySystemComponent* ASC = Binding.ASC.Get();
			ASC->AddAttributeSetSubobject(Owner->GetHealthSetForTest());
			if (!Test.TestEqual(TEXT("binding exact production HealthSet"),
				ASC->GetSet<UGGYGOHealthSet>(), static_cast<const UGGYGOHealthSet*>(Owner->GetHealthSetForTest())))
			{
				return false;
			}
			const int32 BindingIndex = Bindings.Add(Binding);
			// 与宿主 BindAvatar 相同的两步：原生 InitAbilityActorInfo，再由 PawnExtension 广播就绪。
			ASC->InitAbilityActorInfo(Owner, Avatar);
			const bool bBound = Binding.Extension->HandleAbilitySystemBound(ASC);
			Binding.Resource = Binding.Extension->GetCurrentLocalAbilitySystemResource();
			Bindings[BindingIndex].Resource = Binding.Resource;
			if (!Test.TestTrue(TEXT("binding real ActorInfo and ready Extension resource"),
				bBound && ASC->GetOwnerActor() == Owner && ASC->GetAvatarActor() == Avatar
				&& Binding.Resource.HasResource()
				&& Binding.Resource.GetIdentity().ASC.HasSameIndexAndSerialNumber(Binding.ASC)
				&& Binding.Resource.GetIdentity().Pawn.HasSameIndexAndSerialNumber(TWeakObjectPtr<APawn>(Avatar))
				&& Binding.Extension->IsLocalAbilitySystemResourceReady(Binding.Resource))) { return false; }
			OutBinding = Binding;
			return true;
		}

		void ReleaseBinding(const FDeathProjectionBinding& Binding)
		{
			if (!Test.TestTrue(TEXT("cleanup original endpoints remain available"),
				AreOriginalEndpointsCurrent(Binding))) { return; }
			UGGYGOAbilitySystemComponent* ASC = Binding.ASC.Get();
			if (Binding.Resource.HasResource())
			{
				FString Error;
				const bool bHealthReleased = Binding.Avatar->GetHealthForTest()->UninitializeFromLocalAbilitySystemResource(
					Binding.Resource, Error);
				Test.TestTrue(FString::Printf(TEXT("cleanup original Health resource: %s"), *Error), bHealthReleased);
				Binding.Extension->HandleAbilitySystemReleased();
				Test.TestFalse(TEXT("cleanup releases the original Extension resource"),
					Binding.Extension->IsLocalAbilitySystemResourceReady(Binding.Resource));
			}
			ASC->InitAbilityActorInfo(Binding.Owner.Get(), nullptr);
		}

		~FCombatantDeathTestWorld()
		{
			for (const FDeathProjectionBinding& Binding : Bindings) { ReleaseBinding(Binding); }
			Bindings.Reset();
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
	FCombatantDeathTestWorld Fixture(*this);
	if (!TestNotNull(TEXT("test world"), Fixture.World)) { return false; }

	AGGYGOCombatantDeathProjectionTestPawn* Pawn =
		Fixture.World->SpawnActor<AGGYGOCombatantDeathProjectionTestPawn>();
	if (!TestNotNull(TEXT("test Pawn"), Pawn)) { return false; }
	UGGYGOAbilitySystemComponent* ASC = Pawn->GetASCForTest();
	UGGYGOHealthComponent* Health = Pawn->GetHealthForTest();
	FDeathProjectionBinding Binding;
	if (!Fixture.BindAvatar(Pawn, Pawn, Binding)) { return false; }
	if (!TestNotNull(TEXT("fixture ASC has production HealthSet"), ASC->GetSet<UGGYGOHealthSet>()))
	{
		return false;
	}
	FString HealthError;
	const bool bHealthInitialized = Health->InitializeWithLocalAbilitySystemResource(
		Binding.Extension.Get(), Binding.Resource, HealthError);
	if (!TestTrue(FString::Printf(TEXT("Health initializes with original Ready H: %s"), *HealthError),
		bHealthInitialized)) { return false; }

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

	const bool bHealthUninitialized = Health->UninitializeFromLocalAbilitySystemResource(Binding.Resource, HealthError);
	if (!TestTrue(FString::Printf(TEXT("Health unbinds exact original H: %s"), *HealthError),
		bHealthUninitialized)) { return false; }
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
	FCombatantDeathTestWorld Fixture(*this);
	if (!TestNotNull(TEXT("test world"), Fixture.World)) { return false; }

	AGGYGOCombatantDeathProjectionTestPawn* LatePawn =
		Fixture.World->SpawnActor<AGGYGOCombatantDeathProjectionTestPawn>();
	if (!TestNotNull(TEXT("late-binding Pawn"), LatePawn)) { return false; }
	UGGYGOAbilitySystemComponent* LateASC = LatePawn->GetASCForTest();
	UGGYGOHealthComponent* LateHealth = LatePawn->GetHealthForTest();
	TStrongObjectPtr<UGGYGOCombatantDeathProjectionTestObserver> LateObserver(
		NewObject<UGGYGOCombatantDeathProjectionTestObserver>(GetTransientPackage()));
	ObserveDeath(LateHealth, LateObserver.Get());
	LateHealth->StartDeath();
	LateHealth->FinishDeath();
	TestEqual(TEXT("unbound death emitted one start event"), LateObserver->GetDeathStartedCount(), 1);
	TestEqual(TEXT("unbound death emitted one finish event"), LateObserver->GetDeathFinishedCount(), 1);
	TestEqual(TEXT("unbound death does not project Dying"), LateASC->GetTagCount(GGYGOGameplayTags::State_Dying), 0);
	TestEqual(TEXT("unbound death does not project Dead"), LateASC->GetTagCount(GGYGOGameplayTags::State_Dead), 0);
	FDeathProjectionBinding LateBinding;
	if (!Fixture.BindAvatar(LatePawn, LatePawn, LateBinding)) { return false; }
	if (!TestNotNull(TEXT("late fixture ASC has production HealthSet"),
		LateASC->GetSet<UGGYGOHealthSet>()))
	{
		return false;
	}
	FString HealthError;
	const bool bLateHealthInitialized = LateHealth->InitializeWithLocalAbilitySystemResource(
		LateBinding.Extension.Get(), LateBinding.Resource, HealthError);
	if (!TestTrue(FString::Printf(TEXT("late Health initializes with original Ready H: %s"), *HealthError),
		bLateHealthInitialized)) { return false; }
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
	FDeathProjectionBinding OtherBinding;
	if (!Fixture.BindAvatar(MismatchedOwner, OtherAvatar, OtherBinding)) { return false; }
	if (!TestNotNull(TEXT("mismatched fixture ASC has production HealthSet"),
		MismatchedASC->GetSet<UGGYGOHealthSet>()))
	{
		return false;
	}
	if (!TestTrue(TEXT("identity rejection uses actual OtherAvatar ActorInfo"),
		MismatchedASC->GetOwnerActor() == MismatchedOwner && MismatchedASC->GetAvatarActor() == OtherAvatar)
		|| !TestTrue(TEXT("identity rejection uses published Ready OtherAvatar H"),
			OtherBinding.Resource.HasResource()
			&& OtherBinding.Extension->IsLocalAbilitySystemResourceReady(OtherBinding.Resource))) { return false; }
	const bool bMismatchedHealthInitialized = MismatchedHealth->InitializeWithLocalAbilitySystemResource(
		OtherBinding.Extension.Get(), OtherBinding.Resource, HealthError);
	if (!TestFalse(TEXT("non-owner Health rejects real OtherAvatar H"), bMismatchedHealthInitialized)
		|| !TestTrue(FString::Printf(TEXT("non-owner Health reports exact identity rejection: %s"), *HealthError),
			HealthError.Contains(TEXT("Reason='OwnerOrExtensionMismatch'")))) { return false; }
	MismatchedHealth->StartDeath();
	MismatchedHealth->FinishDeath();
	TestEqual(TEXT("non-owner Avatar blocks Dying projection"),
		MismatchedASC->GetTagCount(GGYGOGameplayTags::State_Dying), 0);
	TestEqual(TEXT("non-owner Avatar blocks Dead projection"),
		MismatchedASC->GetTagCount(GGYGOGameplayTags::State_Dead), 0);

	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS

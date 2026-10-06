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
	// Retained cleanup inputs come only from actual production commits and local installation.
	struct FDeathProjectionBinding
	{
		TWeakObjectPtr<AGGYGOCombatantDeathProjectionTestPawn> Owner;
		TWeakObjectPtr<AGGYGOCombatantDeathProjectionTestPawn> Avatar;
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
		TWeakObjectPtr<UGGYGOPawnExtensionComponent> Extension;
		FGGYGOPawnASCResourceHandle Resource;
		FGGYGOAvatarBindingContext Context;
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
			FGGYGOAvatarBindingRequest Request;
			Request.Kind = EGGYGOAvatarBindingKind::Init;
			Request.OwnerActor = Owner;
			Request.AvatarActor = Avatar;
			Request.IsRequestContextCurrent = IsOriginalCallerCurrent;
			FGGYGOAvatarBindingPublicationReceipt Receipt;
			const FGGYGOAvatarBindingResult Commit = ASC->TryBootstrapAvatarActorInfoTransaction(Request, Receipt);
			if (Commit.bCommitted)
			{
				Binding.Context = Commit.CommittedContext;
				Bindings[BindingIndex].Context = Binding.Context;
			}
			if (!Test.TestTrue(TEXT("binding real native Bootstrap commit"),
				Commit.Outcome == EGGYGOAvatarBindingOutcome::Succeeded && Commit.bCommitted
				&& Binding.Context.HasIssuedContext())) { return false; }
			const FGGYGOPawnASCLocalResult Installed = Binding.Extension->InstallLocalAbilitySystemResources(
				ASC, Avatar, Binding.Context);
			Binding.Resource = Installed.Resource;
			Bindings[BindingIndex].Resource = Binding.Resource;
			if (!Test.TestTrue(TEXT("binding real original Extension H"),
				Installed.Outcome == EGGYGOPawnASCLocalOutcome::Succeeded && Binding.Resource.HasResource()
				&& Binding.Resource.GetIdentity().ASC.HasSameIndexAndSerialNumber(Binding.ASC)
				&& Binding.Resource.GetIdentity().Pawn.HasSameIndexAndSerialNumber(TWeakObjectPtr<APawn>(Avatar))
				&& Binding.Resource.GetIdentity().Binding.HasSameIdentity(Binding.Context.Binding))
				|| !Test.TestFalse(TEXT("binding installation alone is not Ready"),
					Binding.Extension->IsLocalAbilitySystemResourceReady(Binding.Resource))) { return false; }

			int32 NoticeCount = 0;
			bool bReadyAccepted = false;
			const FDelegateHandle Bridge = ASC->OnAvatarBindingNotice().AddLambda(
				[this, Binding, Receipt, &NoticeCount, &bReadyAccepted](
					const FGGYGOAvatarBindingPublicationReceipt& DeliveredReceipt,
					const FGGYGOAvatarBindingNotice& Notice)
				{
					++NoticeCount;
					if (!Test.TestTrue(TEXT("binding actual original Initialized Dispatching"),
						AreOriginalEndpointsCurrent(Binding)
						&& Notice.Kind == EGGYGOAvatarBindingNoticeKind::Initialized
						&& Notice.After.HasSameContext(Binding.Context)
						&& Notice.OwnerActor.Get() == Binding.Owner.Get()
						&& Notice.AvatarActor.Get() == Binding.Avatar.Get()
						&& Binding.ASC->IsAvatarBindingNoticeDispatching(Receipt)
						&& Binding.ASC->IsAvatarBindingNoticeDispatching(DeliveredReceipt))) { return; }
					const FGGYGOPawnASCLocalResult Ready =
						Binding.Extension->NotifyLocalResourcesReady(Binding.Resource, DeliveredReceipt);
					bReadyAccepted = Test.TestTrue(TEXT("binding real H Ready from delivered Receipt"),
						Ready.Outcome == EGGYGOPawnASCLocalOutcome::Succeeded
						&& Ready.Resource.HasSameResource(Binding.Resource)
						&& Binding.Extension->IsLocalAbilitySystemResourceReady(Binding.Resource));
				});
			ON_SCOPE_EXIT
			{
				if (UGGYGOAbilitySystemComponent* OriginalASC = Binding.ASC.Get())
				{
					OriginalASC->OnAvatarBindingNotice().Remove(Bridge);
				}
			};
			if (!Test.TestTrue(TEXT("binding original ASC bridge token"), Bridge.IsValid())) { return false; }
			const auto IsOriginalPublicationCurrent = [Binding]()
			{
				return AreOriginalEndpointsCurrent(Binding)
					&& Binding.Extension->IsLocalAbilitySystemResourceInstalled(Binding.Resource);
			};
			const FGGYGOAvatarBindingResult Published =
				ASC->PublishAvatarBindingNotice(Receipt, IsOriginalPublicationCurrent);
			if (!Test.TestTrue(TEXT("binding real native Publish completed"),
				Published.Outcome == EGGYGOAvatarBindingOutcome::Succeeded && Published.bCommitted
				&& Published.CommittedContext.HasSameContext(Binding.Context))
				|| !Test.TestEqual(TEXT("binding exactly one original notice"), NoticeCount, 1)
				|| !Test.TestTrue(TEXT("binding original Ready bridge accepted"), bReadyAccepted)) { return false; }
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
				Test.TestTrue(FString::Printf(TEXT("cleanup original Health H: %s"), *Error), bHealthReleased);
				const FGGYGOPawnASCLocalResult Withdrawn =
					Binding.Extension->WithdrawLocalAbilitySystemResources(Binding.Resource);
				if (!Test.TestTrue(TEXT("cleanup exact original H withdrawal"),
					Withdrawn.Outcome == EGGYGOPawnASCLocalOutcome::Succeeded
					&& Withdrawn.Resource.HasSameResource(Binding.Resource))) { return; }
				const FGGYGOPawnASCLocalResult Released =
					Binding.Extension->NotifyLocalResourcesReleased(Binding.Resource);
				if (!Test.TestTrue(TEXT("cleanup exact original H Released"),
					Released.Outcome == EGGYGOPawnASCLocalOutcome::Succeeded
					&& Released.Resource.HasSameResource(Binding.Resource))) { return; }
			}
			if (!Binding.Context.HasIssuedContext()) { return; }
			FGGYGOAvatarBindingRequest Clear;
			Clear.Kind = EGGYGOAvatarBindingKind::Clear;
			Clear.ExpectedContext = Binding.Context;
			Clear.ClearMode = EGGYGOAvatarBindingClearMode::PreserveOwner;
			Clear.IsRequestContextCurrent = [Binding]() { return AreOriginalEndpointsCurrent(Binding); };
			FGGYGOAvatarBindingPublicationReceipt Receipt;
			const FGGYGOAvatarBindingResult Cleared = ASC->TryExecuteAvatarActorInfoTransaction(Clear, Receipt);
			if (!Test.TestTrue(TEXT("cleanup exact original Context native Clear"),
				Cleared.Outcome == EGGYGOAvatarBindingOutcome::Succeeded && Cleared.bCommitted)) { return; }
			const auto IsClearPublicationCurrent = [Binding, Context = Cleared.CommittedContext]()
			{
				EGGYGOAvatarBindingReason Reason;
				return AreOriginalEndpointsCurrent(Binding)
					&& Binding.ASC->GetOwnerActor() == Binding.Owner.Get()
					&& Binding.ASC->GetAvatarActor() == nullptr
					&& Binding.ASC->CheckAvatarBindingContext(Context, Reason) == EGGYGOAvatarBindingOutcome::Succeeded;
			};
			const FGGYGOAvatarBindingResult Published = ASC->PublishAvatarBindingNotice(Receipt, IsClearPublicationCurrent);
			Test.TestTrue(TEXT("cleanup actual original Clear publication"),
				Published.Outcome == EGGYGOAvatarBindingOutcome::Succeeded && Published.bCommitted);
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
		Binding.Extension.Get(), Binding.Resource, Binding.Context, HealthError);
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
		LateBinding.Extension.Get(), LateBinding.Resource, LateBinding.Context, HealthError);
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
			&& OtherBinding.Extension->IsLocalAbilitySystemResourceReady(OtherBinding.Resource)
			&& MismatchedASC->IsAvatarBindingPublicationContextCurrent(OtherBinding.Context))) { return false; }
	const bool bMismatchedHealthInitialized = MismatchedHealth->InitializeWithLocalAbilitySystemResource(
		OtherBinding.Extension.Get(), OtherBinding.Resource, OtherBinding.Context, HealthError);
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

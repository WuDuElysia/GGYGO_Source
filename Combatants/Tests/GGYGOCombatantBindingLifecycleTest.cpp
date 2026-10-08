#include "Combatants/Tests/GGYGOCombatantBindingLifecycleTestTypes.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCombatantBindingLifecycleTestTypes)

AGGYGOCombatantBindingTestState::AGGYGOCombatantBindingTestState(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

AGGYGOCombatantBindingTestPawn::AGGYGOCombatantBindingTestPawn(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	TestRoot = CreateDefaultSubobject<USceneComponent>(TEXT("TestRoot"));
	SetRootComponent(TestRoot);
	PawnExtension = CreateDefaultSubobject<UGGYGOPawnExtensionComponent>(TEXT("PawnExtension"));
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Components/GameFrameworkComponentManager.h"
#include "Engine/GameInstance.h"
#include "Engine/NetworkDelegates.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "Templates/SharedPointer.h"

namespace
{
	struct FCombatantBindingTestWorld
	{
		UEngine* Engine = nullptr;
		UWorld* World = nullptr;
		TArray<TPair<TWeakObjectPtr<UGGYGOPawnExtensionComponent>, FDelegateHandle>> LocalNoticeHandles;

		FCombatantBindingTestWorld()
			: Engine(GEngine)
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

		~FCombatantBindingTestWorld()
		{
			for (const auto& Registration : LocalNoticeHandles)
			{
				if (UGGYGOPawnExtensionComponent* Extension = Registration.Key.Get())
				{
					Extension->UnregisterLocalAbilitySystemNotice(Registration.Value);
				}
			}
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

	AGGYGOCombatantBindingTestState* SpawnHost(UWorld* World)
	{
		return World ? World->SpawnActor<AGGYGOCombatantBindingTestState>() : nullptr;
	}

	AGGYGOCombatantBindingTestPawn* SpawnPawn(UWorld* World)
	{
		return World ? World->SpawnActor<AGGYGOCombatantBindingTestPawn>() : nullptr;
	}

	void ObserveUninitialization(FCombatantBindingTestWorld& Fixture, UGGYGOPawnExtensionComponent* Extension,
		UGGYGOCombatantBindingTestObserver* Observer)
	{
		const FDelegateHandle Handle = Extension->RegisterLocalAbilitySystemNoticeAndCall(
			FGGYGOPawnASCLocalNoticeDelegate::FDelegate::CreateWeakLambda(Observer,
				[Observer](const FGGYGOPawnASCLocalNotice& Notice)
				{
					if (Notice.Kind == EGGYGOPawnASCLocalNoticeKind::Released) { Observer->HandleUninitialized(); }
				}));
		Fixture.LocalNoticeHandles.Emplace(TWeakObjectPtr<UGGYGOPawnExtensionComponent>(Extension), Handle);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCombatantBindingOwnershipTest,
	"GGYGO.Combatants.Binding.OwnershipAndDeathGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCombatantBindingOwnershipTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FCombatantBindingTestWorld Fixture;
	if (!TestNotNull(TEXT("test world"), Fixture.World)) { return false; }

	AGGYGOCombatantBindingTestState* FirstHost = SpawnHost(Fixture.World);
	AGGYGOCombatantBindingTestState* SecondHost = SpawnHost(Fixture.World);
	AGGYGOCombatantBindingTestPawn* FirstPawn = SpawnPawn(Fixture.World);
	AGGYGOCombatantBindingTestPawn* SecondPawn = SpawnPawn(Fixture.World);
	if (!TestNotNull(TEXT("first host"), FirstHost)
		|| !TestNotNull(TEXT("second host"), SecondHost)
		|| !TestNotNull(TEXT("first pawn"), FirstPawn)
		|| !TestNotNull(TEXT("second pawn"), SecondPawn))
	{
		return false;
	}

	UGGYGOAbilitySystemComponent* FirstASC = FirstHost->GetGGYGOAbilitySystemComponent();
	UGGYGOPawnExtensionComponent* FirstExtension = FirstPawn->GetPawnExtensionForTest();

	FirstHost->AttachAvatar(FirstPawn);
	TestEqual(TEXT("first host owns the Pawn"), FirstHost->GetAvatarPawn(), static_cast<APawn*>(FirstPawn));
	TestEqual(TEXT("Pawn caches first host ASC"), FirstExtension->GetGGYGOAbilitySystemComponent(), FirstASC);

	AddExpectedError(TEXT("已缓存另一 ASC"), EAutomationExpectedErrorFlags::Contains, 1);
	SecondHost->AttachAvatar(FirstPawn);
	TestNull(TEXT("cross-host takeover is rejected before changing the second host"), SecondHost->GetAvatarPawn());
	TestEqual(TEXT("rejected takeover preserves the original local binding"),
		FirstExtension->GetGGYGOAbilitySystemComponent(), FirstASC);
	TestEqual(TEXT("rejected takeover preserves the original shared Avatar"),
		FirstASC->GetAvatarActor(), static_cast<AActor*>(FirstPawn));

	FirstASC->SetLooseGameplayTagCount(GGYGOGameplayTags::State_Dying, 1);
	FirstHost->AttachAvatar(FirstPawn);
	TestEqual(TEXT("dead host may repeat the same idempotent attach"),
		FirstHost->GetAvatarPawn(), static_cast<APawn*>(FirstPawn));
	AddExpectedError(TEXT("死亡状态宿主"), EAutomationExpectedErrorFlags::Contains, 1);
	FirstHost->AttachAvatar(SecondPawn);
	TestEqual(TEXT("dead host rejects a new Avatar before detaching the current one"),
		FirstHost->GetAvatarPawn(), static_cast<APawn*>(FirstPawn));
	TestNull(TEXT("rejected dead-host attach leaves the candidate unbound"),
		SecondPawn->GetPawnExtensionForTest()->GetGGYGOAbilitySystemComponent());

	FirstASC->SetLooseGameplayTagCount(GGYGOGameplayTags::State_Dying, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCombatantBindingIdentityTest,
	"GGYGO.Combatants.Binding.ExpectedASCAndEndPlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCombatantBindingIdentityTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FCombatantBindingTestWorld Fixture;
	if (!TestNotNull(TEXT("test world"), Fixture.World)) { return false; }

	AGGYGOCombatantBindingTestState* OldHost = SpawnHost(Fixture.World);
	AGGYGOCombatantBindingTestState* NewHost = SpawnHost(Fixture.World);
	AGGYGOCombatantBindingTestPawn* Pawn = SpawnPawn(Fixture.World);
	if (!TestNotNull(TEXT("old host"), OldHost)
		|| !TestNotNull(TEXT("new host"), NewHost)
		|| !TestNotNull(TEXT("Pawn"), Pawn))
	{
		return false;
	}

	UGGYGOAbilitySystemComponent* OldASC = OldHost->GetGGYGOAbilitySystemComponent();
	UGGYGOAbilitySystemComponent* NewASC = NewHost->GetGGYGOAbilitySystemComponent();
	UGGYGOPawnExtensionComponent* Extension = Pawn->GetPawnExtensionForTest();
	OldHost->AttachAvatar(Pawn);
	const FGGYGOPawnASCResourceHandle OldResource = Extension->GetCurrentLocalAbilitySystemResource();
	bool bOldBindingReady = TestTrue(TEXT("transfer uses distinct persistent ASCs"), OldASC != NewASC);
	bOldBindingReady &= TestEqual(TEXT("transfer starts with the old Host selecting Pawn"),
		OldHost->GetAvatarPawn(), static_cast<APawn*>(Pawn));
	bOldBindingReady &= TestEqual(TEXT("transfer starts with the old shared Avatar"),
		OldASC->GetAvatarActor(), static_cast<AActor*>(Pawn));
	bOldBindingReady &= TestEqual(TEXT("transfer starts with the old local ASC"),
		Extension->GetGGYGOAbilitySystemComponent(), OldASC);
	bOldBindingReady &= TestTrue(TEXT("transfer starts with a real Ready old H"),
		OldResource.HasResource() && Extension->IsLocalAbilitySystemResourceReady(OldResource));
	if (!bOldBindingReady) { return false; }

	// Cross-Host transfer explicitly releases the old resource before binding the new Host.
	OldHost->DetachAvatar(Pawn);
	bool bOldBindingReleased = TestTrue(TEXT("explicit old release withdraws the old local H"),
		!Extension->GetCurrentLocalAbilitySystemResource().HasResource()
		&& !Extension->IsLocalAbilitySystemResourceInstalled(OldResource));
	bOldBindingReleased &= TestNull(TEXT("explicit old release clears the old Host selection"),
		OldHost->GetAvatarPawn());
	bOldBindingReleased &= TestNull(TEXT("explicit old release clears the old shared Avatar"),
		OldASC->GetAvatarActor());
	bOldBindingReleased &= TestNull(TEXT("explicit old release clears the old local ASC"),
		Extension->GetGGYGOAbilitySystemComponent());
	if (!bOldBindingReleased) { return false; }

	NewHost->AttachAvatar(Pawn);
	const FGGYGOPawnASCResourceHandle NewResource = Extension->GetCurrentLocalAbilitySystemResource();
	bool bNewBindingReady = TestEqual(TEXT("new Host really selects Pawn before late cleanup"),
		NewHost->GetAvatarPawn(), static_cast<APawn*>(Pawn));
	bNewBindingReady &= TestEqual(TEXT("new ASC really names the new Host before late cleanup"),
		NewASC->GetOwnerActor(), static_cast<AActor*>(NewHost));
	bNewBindingReady &= TestEqual(TEXT("new shared Avatar is established before late cleanup"),
		NewASC->GetAvatarActor(), static_cast<AActor*>(Pawn));
	bNewBindingReady &= TestEqual(TEXT("new local ASC is established before late cleanup"),
		Extension->GetGGYGOAbilitySystemComponent(), NewASC);
	bNewBindingReady &= TestTrue(TEXT("new binding publishes a distinct real Ready H before late cleanup"),
		NewResource.HasResource() && !NewResource.HasSameResource(OldResource)
		&& NewResource.GetIdentity().ASC.Get() == NewASC
		&& NewResource.GetIdentity().Pawn.Get() == Pawn
		&& Extension->IsLocalAbilitySystemResourceReady(NewResource));
	if (!bNewBindingReady) { return false; }

	TStrongObjectPtr<UGGYGOCombatantBindingTestObserver> Observer(
		NewObject<UGGYGOCombatantBindingTestObserver>(GetTransientPackage()));
	ObserveUninitialization(Fixture, Extension, Observer.Get());

	OldHost->DetachAvatar(Pawn);
	TestEqual(TEXT("late old-host detach preserves the Pawn's new local ASC"),
		Extension->GetGGYGOAbilitySystemComponent(), NewASC);
	TestEqual(TEXT("late old-host detach preserves the new shared Avatar"),
		NewASC->GetAvatarActor(), static_cast<AActor*>(Pawn));
	TestEqual(TEXT("ExpectedASC mismatch does not broadcast"), Observer->GetUninitializedCount(), 0);
	TestNull(TEXT("late old-host detach leaves its already released Avatar clear"), OldASC->GetAvatarActor());

	// Recreate the delayed identity cleanup directly: OldASC still names Pawn, while the
	// PawnExtension local cache already owns NewASC. ExpectedASC mismatch must be a no-op.
	OldASC->InitAbilityActorInfo(OldHost, Pawn);
	Extension->UninitializeAbilitySystem(OldASC);
	TestEqual(TEXT("late ExistingAvatar cleanup cannot clear OtherASC"),
		Extension->GetGGYGOAbilitySystemComponent(), NewASC);
	TestEqual(TEXT("OtherASC retains its Avatar"), NewASC->GetAvatarActor(), static_cast<AActor*>(Pawn));
	TestEqual(TEXT("mismatched ExistingAvatar cleanup still does not broadcast"),
		Observer->GetUninitializedCount(), 0);
	TestEqual(TEXT("mismatched PawnExtension cleanup does not mutate the expected ASC"),
		OldASC->GetAvatarActor(), static_cast<AActor*>(Pawn));

	AGGYGOCombatantBindingTestState* EndPlayHost = SpawnHost(Fixture.World);
	AGGYGOCombatantBindingTestPawn* EndPlayPawn = SpawnPawn(Fixture.World);
	if (!TestNotNull(TEXT("EndPlay host"), EndPlayHost)
		|| !TestNotNull(TEXT("EndPlay Pawn"), EndPlayPawn))
	{
		return false;
	}
	UGGYGOPawnExtensionComponent* EndPlayExtension = EndPlayPawn->GetPawnExtensionForTest();
	TStrongObjectPtr<UGGYGOCombatantBindingTestObserver> EndPlayObserver(
		NewObject<UGGYGOCombatantBindingTestObserver>(GetTransientPackage()));
	ObserveUninitialization(Fixture, EndPlayExtension, EndPlayObserver.Get());
	EndPlayHost->AttachAvatar(EndPlayPawn);
	EndPlayHost->InvokeEndPlayForTest();
	TestNull(TEXT("host EndPlay clears the Pawn's local ASC cache"),
		EndPlayExtension->GetGGYGOAbilitySystemComponent());
	TestEqual(TEXT("host EndPlay broadcasts one local uninitialization"),
		EndPlayObserver->GetUninitializedCount(), 1);

	return true;
}

namespace
{
	/** Values captured while the destroyed host is still valid, before MarkAsGarbage. */
	struct FCombatantRealDestroySnapshot
	{
		bool bHostValid = false;
		bool bHostBeingDestroyed = false;
		bool bHostHasBegunPlay = false;
		APawn* ReplicatedAvatar = nullptr;
		AActor* OwnerActor = nullptr;
		AActor* ASCAvatar = nullptr;
		UGGYGOAbilitySystemComponent* OldCachedASC = nullptr;
		UGGYGOAbilitySystemComponent* CandidateCachedASC = nullptr;
		bool bOldDestroyedSubscription = false;
		bool bCandidateDestroyedSubscription = false;
	};

	/** Owns a real playing World and only observes the synchronous native Destroy window. */
	struct FCombatantRealDestroyFixture : TSharedFromThis<FCombatantRealDestroyFixture>
	{
		UEngine* Engine = GEngine;
		TStrongObjectPtr<UGameInstance> GameInstance;
		UWorld* World = nullptr;
		UGameFrameworkComponentManager* Manager = nullptr;
		AGGYGOCombatantBindingTestState* Host = nullptr;
		AGGYGOCombatantBindingTestState* OtherHost = nullptr;
		AGGYGOCombatantBindingTestPawn* OldPawn = nullptr;
		AGGYGOCombatantBindingTestPawn* CandidatePawn = nullptr;
		UGGYGOAbilitySystemComponent* ASC = nullptr;
		UGGYGOAbilitySystemComponent* OtherASC = nullptr;
		UGGYGOPawnExtensionComponent* OldExtension = nullptr;
		UGGYGOPawnExtensionComponent* CandidateExtension = nullptr;
		bool bGameInstanceInitialized = false;
		bool bProbeArmed = false;
		FDelegateHandle DestroyedHandle;
		FDelegateHandle RemovedHandle;
		FDelegateHandle OldNoticeHandle;
		FDelegateHandle CandidateNoticeHandle;
		FNetDelegates::FReceivedNetworkEncryptionToken SavedEncryptionToken = FNetDelegates::OnReceivedNetworkEncryptionToken;
		FNetDelegates::FReceivedNetworkEncryptionAck SavedEncryptionAck = FNetDelegates::OnReceivedNetworkEncryptionAck;
		FNetDelegates::FReceivedNetworkEncryptionFailure SavedEncryptionFailure = FNetDelegates::OnReceivedNetworkEncryptionFailure;
		int32 DestroyedCount = 0;
		int32 UninitializedCount = 0;
		int32 ReattachAttemptCount = 0;
		int32 CandidateInitializedCount = 0;
		int32 RemovedCount = 0;
		TArray<FName> Events;
		FCombatantRealDestroySnapshot NativeDestroy;
		FCombatantRealDestroySnapshot CleanupBeforeAttach;
		FCombatantRealDestroySnapshot CleanupAfterAttach;
		FCombatantRealDestroySnapshot Removed;

		bool CheckPlayingActor(FAutomationTestBase& Test, AActor* Actor) const
		{
			const FString Prefix = Actor->GetName() + TEXT(": ");
			bool bPassed = Test.TestTrue(Prefix + TEXT("real initialized authority Actor completed BeginPlay"),
				Actor->IsActorInitialized() && Actor->HasAuthority() && Actor->HasActorBegunPlay()
				&& !Actor->IsActorBeginningPlay() && !Actor->IsActorBeingDestroyed());
			TInlineComponentArray<UActorComponent*> Components(Actor);
			for (UActorComponent* Component : Components)
			{
				bPassed &= Test.TestTrue(Prefix + Component->GetName() + TEXT(" registered and began play"),
					Component->IsRegistered() && Component->HasBegunPlay());
			}
			return bPassed;
		}

		bool Initialize(FAutomationTestBase& Test)
		{
			if (!Test.TestNotNull(TEXT("real Engine"), Engine)) { return false; }
			GameInstance.Reset(NewObject<UGameInstance>(Engine, NAME_None, RF_Transient));
			if (!Test.TestNotNull(TEXT("base GameInstance"), GameInstance.Get())) { return false; }
			bGameInstanceInitialized = true;
			GameInstance->InitializeStandalone(FName(TEXT("GGYGOCombatantRealDestroyTestWorld")));
			World = GameInstance->GetWorld();
			if (!Test.TestNotNull(TEXT("GI-owned real World"), World)) { return false; }
			const FWorldContext* Context = Engine->GetWorldContextFromWorld(World);
			if (!Test.TestTrue(TEXT("private initialized Game World and owning GI context"),
				World->IsGameWorld() && World->IsInitialized() && World->GetGameInstance() == GameInstance.Get()
				&& Context && Context->World() == World && Context->OwningGameInstance == GameInstance.Get()
				&& GameInstance->GetWorldContext() == Context && GameInstance->GetLocalPlayers().IsEmpty()))
			{
				return false;
			}
			Manager = UGameInstance::GetSubsystem<UGameFrameworkComponentManager>(GameInstance.Get());
			if (!Test.TestNotNull(TEXT("real ComponentManager from base GI Init"), Manager)) { return false; }

			FURL PlayURL;
			PlayURL.AddOption(*FString::Printf(TEXT("game=%s"), *AGameModeBase::StaticClass()->GetPathName()));
			if (!Test.TestTrue(TEXT("explicit native BaseGameMode creation"), World->SetGameMode(PlayURL)))
			{
				return false;
			}
			AGameModeBase* GameMode = World->GetAuthGameMode();
			if (!Test.TestTrue(TEXT("actual GameMode is exactly the requested native class"),
				GameMode && GameMode->GetClass() == AGameModeBase::StaticClass()))
			{
				return false;
			}
			World->InitializeActorsForPlay(PlayURL);
			if (!Test.TestTrue(TEXT("real Actor initialization precedes World BeginPlay"),
				World->AreActorsInitialized() && !World->HasBegunPlay())
				|| !Test.TestNotNull(TEXT("native GameState for StartPlay"), World->GetGameState()))
			{
				return false;
			}

			Host = SpawnHost(World);
			OtherHost = SpawnHost(World);
			OldPawn = SpawnPawn(World);
			CandidatePawn = SpawnPawn(World);
			if (!Test.TestNotNull(TEXT("destroy target host"), Host)
				|| !Test.TestNotNull(TEXT("ExpectedASC mismatch host"), OtherHost)
				|| !Test.TestNotNull(TEXT("old Avatar"), OldPawn)
				|| !Test.TestNotNull(TEXT("unbound candidate Avatar"), CandidatePawn))
			{
				return false;
			}
			ASC = Host->GetGGYGOAbilitySystemComponent();
			OtherASC = OtherHost->GetGGYGOAbilitySystemComponent();
			OldExtension = OldPawn->GetPawnExtensionForTest();
			CandidateExtension = CandidatePawn->GetPawnExtensionForTest();
			if (!Test.TestNotNull(TEXT("host ASC"), ASC)
				|| !Test.TestNotNull(TEXT("other host ASC"), OtherASC)
				|| !Test.TestNotNull(TEXT("old real PawnExtension"), OldExtension)
				|| !Test.TestNotNull(TEXT("candidate real PawnExtension"), CandidateExtension))
			{
				return false;
			}

			World->BeginPlay();
			if (!Test.TestTrue(TEXT("native World/GameMode/GameState completed BeginPlay"),
				World->HasBegunPlay() && GameMode->HasActorBegunPlay() && World->GetGameState()->HasActorBegunPlay()))
			{
				return false;
			}
			bool bPassed = CheckPlayingActor(Test, Host);
			bPassed &= CheckPlayingActor(Test, OtherHost);
			bPassed &= CheckPlayingActor(Test, OldPawn);
			bPassed &= CheckPlayingActor(Test, CandidatePawn);
			bPassed &= Test.TestTrue(TEXT("both persistent ASCs initialized with their own Owner and no Avatar"),
				ASC != OtherASC && ASC->HasBeenInitialized() && OtherASC->HasBeenInitialized()
				&& ASC->GetOwnerActor() == Host && OtherASC->GetOwnerActor() == OtherHost
				&& !ASC->GetAvatarActor() && !OtherASC->GetAvatarActor()
				&& !Host->GetAvatarPawn() && !OtherHost->GetAvatarPawn());
			bPassed &= Test.TestTrue(TEXT("both real PawnExtensions use this GI manager and reached Spawned"),
				UGameFrameworkComponentManager::GetForActor(OldPawn) == Manager
				&& UGameFrameworkComponentManager::GetForActor(CandidatePawn) == Manager
				&& OldExtension->GetInitState() == GGYGOGameplayTags::InitState_Spawned
				&& CandidateExtension->GetInitState() == GGYGOGameplayTags::InitState_Spawned
				&& !OldExtension->GetGGYGOAbilitySystemComponent()
				&& !CandidateExtension->GetGGYGOAbilitySystemComponent());
			bPassed &= Test.TestNull(TEXT("old Pawn has no second Pawn-owned ASC"),
				OldPawn->FindComponentByClass<UGGYGOAbilitySystemComponent>());
			bPassed &= Test.TestNull(TEXT("candidate Pawn has no second Pawn-owned ASC"),
				CandidatePawn->FindComponentByClass<UGGYGOAbilitySystemComponent>());
			return bPassed;
		}

		FCombatantRealDestroySnapshot Capture() const
		{
			FCombatantRealDestroySnapshot Snapshot;
			Snapshot.bHostValid = IsValid(Host);
			Snapshot.bHostBeingDestroyed = Host->IsActorBeingDestroyed();
			Snapshot.bHostHasBegunPlay = Host->HasActorBegunPlay();
			Snapshot.ReplicatedAvatar = Host->GetAvatarPawn();
			Snapshot.OwnerActor = ASC->GetOwnerActor();
			Snapshot.ASCAvatar = ASC->GetAvatarActor();
			Snapshot.OldCachedASC = OldExtension->GetGGYGOAbilitySystemComponent();
			Snapshot.CandidateCachedASC = CandidateExtension->GetGGYGOAbilitySystemComponent();
			const FName HandlerName(TEXT("HandleAvatarDestroyed"));
			Snapshot.bOldDestroyedSubscription = OldPawn->OnDestroyed.Contains(Host, HandlerName);
			Snapshot.bCandidateDestroyedSubscription = CandidatePawn->OnDestroyed.Contains(Host, HandlerName);
			return Snapshot;
		}

		void InstallProbe()
		{
			// CreateSP keeps no strong fixture reference and skips expired receivers.
			DestroyedHandle = World->AddOnActorDestroyedHandler(
				FOnActorDestroyed::FDelegate::CreateSP(AsShared(), &FCombatantRealDestroyFixture::HandleNativeDestroy));
			RemovedHandle = World->AddOnActorRemovedFromWorldHandler(
				FOnActorRemovedFromWorld::FDelegate::CreateSP(AsShared(), &FCombatantRealDestroyFixture::HandleRemoved));
			OldNoticeHandle = OldExtension->RegisterLocalAbilitySystemNoticeAndCall(
				FGGYGOPawnASCLocalNoticeDelegate::FDelegate::CreateSP(AsShared(), &FCombatantRealDestroyFixture::HandleUninitialized));
			CandidateNoticeHandle = CandidateExtension->RegisterLocalAbilitySystemNoticeAndCall(
				FGGYGOPawnASCLocalNoticeDelegate::FDelegate::CreateSP(AsShared(), &FCombatantRealDestroyFixture::HandleCandidateInitialized));
		}

		void HandleNativeDestroy(AActor* Actor)
		{
			if (!bProbeArmed || Actor != Host) { return; }
			++DestroyedCount;
			Events.Add(FName(TEXT("NativeDestroy")));
			NativeDestroy = Capture();
		}

		void HandleUninitialized(const FGGYGOPawnASCLocalNotice& Notice)
		{
			if (Notice.Kind != EGGYGOPawnASCLocalNoticeKind::Released) { return; }
			++UninitializedCount;
			if (!bProbeArmed) { return; }
			Events.Add(FName(TEXT("Uninitialized")));
			CleanupBeforeAttach = Capture();
			++ReattachAttemptCount;
			Host->AttachAvatar(CandidatePawn);
			CleanupAfterAttach = Capture();
		}

		void HandleCandidateInitialized(const FGGYGOPawnASCLocalNotice& Notice)
		{
			if (Notice.Kind != EGGYGOPawnASCLocalNoticeKind::Ready) { return; }
			++CandidateInitializedCount;
		}

		void HandleRemoved(AActor* Actor)
		{
			if (!bProbeArmed || Actor != Host) { return; }
			++RemovedCount;
			Events.Add(FName(TEXT("Removed")));
			Removed = Capture();
		}

		~FCombatantRealDestroyFixture()
		{
			bProbeArmed = false;
			if (IsValid(OldExtension)) { OldExtension->UnregisterLocalAbilitySystemNotice(OldNoticeHandle); }
			if (IsValid(CandidateExtension)) { CandidateExtension->UnregisterLocalAbilitySystemNotice(CandidateNoticeHandle); }
			if (World)
			{
				World->RemoveOnActorDestroyedHandler(DestroyedHandle);
				World->RemoveOnActorRemovedFromWorldHandler(RemovedHandle);
				// Keep the real GI/ComponentManager alive for Actor and component EndPlay on every exit.
				if (IsValid(OldPawn)) { OldPawn->Destroy(); }
				if (IsValid(CandidatePawn)) { CandidatePawn->Destroy(); }
				if (IsValid(Host)) { Host->Destroy(); }
				if (IsValid(OtherHost)) { OtherHost->Destroy(); }
				World->EndPlay(EEndPlayReason::Quit);
			}
			if (bGameInstanceInitialized && GameInstance.IsValid()) { GameInstance->Shutdown(); }
			if (World)
			{
				World->DestroyWorld(false);
				Engine->DestroyWorldContext(World);
				if (UPackage* Package = World->GetPackage()) { Package->SetDirtyFlag(false); }
			}
			// Base GI Init/Shutdown mutate these globals; restore all three original bindings.
			FNetDelegates::OnReceivedNetworkEncryptionToken = SavedEncryptionToken;
			FNetDelegates::OnReceivedNetworkEncryptionAck = SavedEncryptionAck;
			FNetDelegates::OnReceivedNetworkEncryptionFailure = SavedEncryptionFailure;
			GameInstance.Reset();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCombatantRealDestroyReattachTest,
	"GGYGO.Combatants.Binding.RealDestroyRejectsCleanupReattach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCombatantRealDestroyReattachTest::RunTest(const FString& Parameters)
{
	TSharedRef<FCombatantRealDestroyFixture> Fixture = MakeShared<FCombatantRealDestroyFixture>();
	if (!Fixture->Initialize(*this)) { return false; }

	Fixture->Host->AttachAvatar(Fixture->OldPawn);
	const FCombatantRealDestroySnapshot Bound = Fixture->Capture();
	bool bPassed = TestTrue(TEXT("normal live binding exists before native Destroy"),
		Bound.bHostValid && !Bound.bHostBeingDestroyed && Bound.bHostHasBegunPlay
		&& Bound.ReplicatedAvatar == Fixture->OldPawn && Bound.OwnerActor == Fixture->Host
		&& Bound.ASCAvatar == Fixture->OldPawn && Bound.OldCachedASC == Fixture->ASC
		&& !Bound.CandidateCachedASC && Bound.bOldDestroyedSubscription && !Bound.bCandidateDestroyedSubscription);
	if (!bPassed) { return false; }

	Fixture->InstallProbe();
	bPassed &= TestTrue(TEXT("both native World observation handlers installed"),
		Fixture->DestroyedHandle.IsValid() && Fixture->RemovedHandle.IsValid());
	Fixture->OldExtension->UninitializeAbilitySystem(Fixture->OtherASC);
	const FCombatantRealDestroySnapshot AfterMismatch = Fixture->Capture();
	bPassed &= TestTrue(TEXT("ExpectedASC mismatch leaves all observed bindings and subscriptions unchanged"),
		AfterMismatch.bHostValid == Bound.bHostValid
		&& AfterMismatch.bHostBeingDestroyed == Bound.bHostBeingDestroyed
		&& AfterMismatch.bHostHasBegunPlay == Bound.bHostHasBegunPlay
		&& AfterMismatch.ReplicatedAvatar == Bound.ReplicatedAvatar && AfterMismatch.OwnerActor == Bound.OwnerActor
		&& AfterMismatch.ASCAvatar == Bound.ASCAvatar && AfterMismatch.OldCachedASC == Bound.OldCachedASC
		&& AfterMismatch.CandidateCachedASC == Bound.CandidateCachedASC
		&& AfterMismatch.bOldDestroyedSubscription == Bound.bOldDestroyedSubscription
		&& AfterMismatch.bCandidateDestroyedSubscription == Bound.bCandidateDestroyedSubscription);
	bPassed &= TestTrue(TEXT("mismatched expected ASC and its host remain untouched"),
		Fixture->OtherASC->GetOwnerActor() == Fixture->OtherHost && !Fixture->OtherASC->GetAvatarActor()
		&& !Fixture->OtherHost->GetAvatarPawn());
	bPassed &= TestTrue(TEXT("mismatch and probe installation trigger no notifications or Attach attempt"),
		Fixture->DestroyedCount == 0 && Fixture->UninitializedCount == 0 && Fixture->ReattachAttemptCount == 0
		&& Fixture->CandidateInitializedCount == 0 && Fixture->RemovedCount == 0 && Fixture->Events.IsEmpty());
	if (!bPassed) { return false; }

	const FString ExpectedRejection = FString::Printf(
		TEXT("[Combatants] AttachAvatar.Entry: 宿主 [%s] ASC [%s] 拒绝绑定 Avatar [%s]，原因：宿主正在原生销毁流程中。"),
		*GetPathNameSafe(Fixture->Host), *GetPathNameSafe(Fixture->ASC), *GetPathNameSafe(Fixture->CandidatePawn));
	AddExpectedErrorPlain(ExpectedRejection, EAutomationExpectedErrorFlags::Exact, 1);
	Fixture->bProbeArmed = true;
	const bool bDestroyed = Fixture->Host->Destroy();
	Fixture->bProbeArmed = false;

	bPassed &= TestTrue(TEXT("real Actor::Destroy succeeds"), bDestroyed);
	bPassed &= TestEqual(TEXT("native World destruction notification occurs once"), Fixture->DestroyedCount, 1);
	bPassed &= TestEqual(TEXT("old Extension uninitialization occurs once"), Fixture->UninitializedCount, 1);
	bPassed &= TestEqual(TEXT("only cleanup callback attempts one Attach"), Fixture->ReattachAttemptCount, 1);
	bPassed &= TestEqual(TEXT("native post-EndPlay removal notification occurs once"), Fixture->RemovedCount, 1);
	bPassed &= TestEqual(TEXT("candidate never broadcasts initialized"), Fixture->CandidateInitializedCount, 0);
	const TArray<FName> ExpectedEvents{FName(TEXT("NativeDestroy")), FName(TEXT("Uninitialized")), FName(TEXT("Removed"))};
	bPassed &= TestTrue(TEXT("actual native destruction, cleanup and post-EndPlay removal order"),
		Fixture->Events == ExpectedEvents);
	bPassed &= TestTrue(TEXT("native Destroy sets BeingDestroyed before routing actual EndPlay"),
		Fixture->NativeDestroy.bHostValid && Fixture->NativeDestroy.bHostBeingDestroyed
		&& Fixture->NativeDestroy.bHostHasBegunPlay);
	bPassed &= TestTrue(TEXT("cleanup callback sees cleared local/ASC binding while retaining Owner"),
		Fixture->CleanupBeforeAttach.bHostValid && Fixture->CleanupBeforeAttach.bHostBeingDestroyed
		&& Fixture->CleanupBeforeAttach.bHostHasBegunPlay && Fixture->CleanupBeforeAttach.OwnerActor == Fixture->Host
		&& !Fixture->CleanupBeforeAttach.ASCAvatar && !Fixture->CleanupBeforeAttach.OldCachedASC
		&& !Fixture->CleanupBeforeAttach.CandidateCachedASC);
	bPassed &= TestTrue(TEXT("rejected callback Attach cannot commit the candidate or replace cleanup identity"),
		Fixture->CleanupAfterAttach.ReplicatedAvatar == Fixture->OldPawn
		&& Fixture->CleanupAfterAttach.OwnerActor == Fixture->Host && !Fixture->CleanupAfterAttach.ASCAvatar
		&& !Fixture->CleanupAfterAttach.OldCachedASC && !Fixture->CleanupAfterAttach.CandidateCachedASC
		&& Fixture->CleanupAfterAttach.bOldDestroyedSubscription && !Fixture->CleanupAfterAttach.bCandidateDestroyedSubscription);
	bPassed &= TestTrue(TEXT("post-EndPlay snapshot is captured while host remains valid before Garbage"),
		Fixture->Removed.bHostValid && Fixture->Removed.bHostBeingDestroyed && !Fixture->Removed.bHostHasBegunPlay);
	bPassed &= TestNull(TEXT("native Owner destruction callback clears ASC Owner before World removal"),
		Fixture->Removed.OwnerActor);
	bPassed &= TestNull(TEXT("replicated source Avatar is cleared before Garbage"), Fixture->Removed.ReplicatedAvatar);
	bPassed &= TestNull(TEXT("ASC Avatar is cleared before Garbage"), Fixture->Removed.ASCAvatar);
	bPassed &= TestNull(TEXT("old Extension cache is cleared before Garbage"), Fixture->Removed.OldCachedASC);
	bPassed &= TestNull(TEXT("candidate Extension stays unbound before Garbage"), Fixture->Removed.CandidateCachedASC);
	bPassed &= TestFalse(TEXT("old Pawn has no stale host OnDestroyed subscription"), Fixture->Removed.bOldDestroyedSubscription);
	bPassed &= TestFalse(TEXT("candidate Pawn has no rejected host OnDestroyed subscription"), Fixture->Removed.bCandidateDestroyedSubscription);
	bPassed &= TestFalse(TEXT("native Destroy marks the host invalid after all observed cleanup"), IsValid(Fixture->Host));
	return bPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS

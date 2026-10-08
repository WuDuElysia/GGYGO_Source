#include "Combatants/Tests/GGYGOCombatantBindingSuccessorTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Combatants/Tests/GGYGOCombatantBindingLifecycleTestTypes.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Components/GameFrameworkComponentManager.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/NetworkDelegates.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "System/GGYGOGameplayTags.h"
#include "Templates/SharedPointer.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr const TCHAR* SuccessorDiagnosticFlag = TEXT("GGYGOCombatantBindingSuccessorDiagnostic");
	constexpr const TCHAR* DifferentPawnCase = TEXT("DifferentPawnTakeover");
	constexpr const TCHAR* SamePawnCase = TEXT("SamePawnRebind");

	struct FBindingSuccessorProbe;

	/** A private real playing World; actor types and production coordination are reused unchanged. */
	struct FBindingSuccessorFixture
	{
		UEngine* Engine = GEngine;
		TStrongObjectPtr<UGameInstance> GameInstance;
		UWorld* World = nullptr;
		AGGYGOCombatantBindingTestState* Host = nullptr;
		AGGYGOCombatantBindingTestPawn* OldPawn = nullptr;
		AGGYGOCombatantBindingTestPawn* CandidatePawn = nullptr;
		UGGYGOAbilitySystemComponent* ASC = nullptr;
		UGGYGOPawnExtensionComponent* OldExtension = nullptr;
		UGGYGOPawnExtensionComponent* CandidateExtension = nullptr;
		TSharedPtr<FBindingSuccessorProbe> Probe;
		FDelegateHandle OldNoticeHandle;
		FDelegateHandle CandidateNoticeHandle;
		bool bGameInstanceInitialized = false;
		FNetDelegates::FReceivedNetworkEncryptionToken SavedEncryptionToken = FNetDelegates::OnReceivedNetworkEncryptionToken;
		FNetDelegates::FReceivedNetworkEncryptionAck SavedEncryptionAck = FNetDelegates::OnReceivedNetworkEncryptionAck;
		FNetDelegates::FReceivedNetworkEncryptionFailure SavedEncryptionFailure = FNetDelegates::OnReceivedNetworkEncryptionFailure;

		bool CheckPlayingActor(FAutomationTestBase& Test, AActor* Actor) const
		{
			const FString Prefix = Actor->GetName() + TEXT(": ");
			bool bPassed = Test.TestTrue(Prefix + TEXT("real authority Actor completed BeginPlay"),
				IsValid(Actor) && Actor->GetWorld() == World && Actor->HasAuthority()
				&& Actor->IsActorInitialized() && Actor->HasActorBegunPlay()
				&& !Actor->IsActorBeginningPlay() && !Actor->IsActorBeingDestroyed());
			TInlineComponentArray<UActorComponent*> Components(Actor);
			for (UActorComponent* Component : Components)
			{
				bPassed &= Test.TestTrue(Prefix + Component->GetName() + TEXT(" registered and began play"),
					IsValid(Component) && Component->IsRegistered() && Component->HasBegunPlay()
					&& !Component->IsBeingDestroyed());
			}
			return bPassed;
		}

		bool Initialize(FAutomationTestBase& Test)
		{
			if (!Test.TestNotNull(TEXT("real GEngine"), Engine)) { return false; }
			GameInstance.Reset(NewObject<UGameInstance>(Engine, NAME_None, RF_Transient));
			if (!Test.TestNotNull(TEXT("private base GameInstance"), GameInstance.Get())) { return false; }
			bGameInstanceInitialized = true;
			GameInstance->InitializeStandalone(FName(TEXT("GGYGOCombatantBindingSuccessorWorld")));
			World = GameInstance->GetWorld();
			if (!Test.TestNotNull(TEXT("GI-owned Game World"), World)) { return false; }
			const FWorldContext* Context = Engine->GetWorldContextFromWorld(World);
			if (!Test.TestTrue(TEXT("private initialized World and its owning GI context"),
				World->IsGameWorld() && World->IsInitialized() && World->GetGameInstance() == GameInstance.Get()
				&& Context && Context->World() == World && Context->OwningGameInstance == GameInstance.Get()
				&& GameInstance->GetWorldContext() == Context && GameInstance->GetLocalPlayers().IsEmpty()))
			{
				return false;
			}
			UGameFrameworkComponentManager* Manager =
				UGameInstance::GetSubsystem<UGameFrameworkComponentManager>(GameInstance.Get());
			if (!Test.TestNotNull(TEXT("real GI ComponentManager"), Manager)) { return false; }

			FURL PlayURL;
			PlayURL.AddOption(*FString::Printf(TEXT("game=%s"), *AGameModeBase::StaticClass()->GetPathName()));
			if (!Test.TestTrue(TEXT("explicit native BaseGameMode creation"), World->SetGameMode(PlayURL)))
			{
				return false;
			}
			AGameModeBase* GameMode = World->GetAuthGameMode();
			if (!Test.TestTrue(TEXT("actual GameMode is exactly the requested class"),
				GameMode && GameMode->GetClass() == AGameModeBase::StaticClass()))
			{
				return false;
			}
			World->InitializeActorsForPlay(PlayURL);
			if (!Test.TestTrue(TEXT("real actor initialization before BeginPlay"),
				World->AreActorsInitialized() && !World->HasBegunPlay())
				|| !Test.TestNotNull(TEXT("native GameState"), World->GetGameState()))
			{
				return false;
			}

			Host = World->SpawnActor<AGGYGOCombatantBindingTestState>();
			OldPawn = World->SpawnActor<AGGYGOCombatantBindingTestPawn>();
			CandidatePawn = World->SpawnActor<AGGYGOCombatantBindingTestPawn>();
			if (!Test.TestNotNull(TEXT("persistent host"), Host)
				|| !Test.TestNotNull(TEXT("old Pawn A"), OldPawn)
				|| !Test.TestNotNull(TEXT("candidate Pawn C"), CandidatePawn))
			{
				return false;
			}
			ASC = Host->GetGGYGOAbilitySystemComponent();
			OldExtension = OldPawn->GetPawnExtensionForTest();
			CandidateExtension = CandidatePawn->GetPawnExtensionForTest();
			if (!Test.TestNotNull(TEXT("host ASC"), ASC)
				|| !Test.TestNotNull(TEXT("real old Extension"), OldExtension)
				|| !Test.TestNotNull(TEXT("real candidate Extension"), CandidateExtension)
				|| !Test.TestNotNull(TEXT("public ActorInfo allocation"), ASC->AbilityActorInfo.Get()))
			{
				return false;
			}

			World->BeginPlay();
			if (!Test.TestTrue(TEXT("native World, GameMode and GameState completed BeginPlay"),
				World->HasBegunPlay() && GameMode->HasActorBegunPlay()
				&& World->GetGameState()->HasActorBegunPlay()))
			{
				return false;
			}
			bool bPassed = CheckPlayingActor(Test, Host);
			bPassed &= CheckPlayingActor(Test, OldPawn);
			bPassed &= CheckPlayingActor(Test, CandidatePawn);
			bPassed &= Test.TestTrue(TEXT("single initialized persistent ASC has its own Owner and no Avatar"),
				ASC->HasBeenInitialized() && ASC->GetOwner() == Host && ASC->GetOwnerActor() == Host
				&& !ASC->GetAvatarActor() && !Host->GetAvatarPawn()
				&& ASC->AbilityActorInfo->AbilitySystemComponent.Get() == ASC
				&& ASC->AbilityActorInfo->OwnerActor.Get() == Host && !ASC->AbilityActorInfo->AvatarActor.IsValid());
			bPassed &= Test.TestTrue(TEXT("real Extensions belong to their Pawns and this GI manager"),
				OldExtension->GetOwner() == OldPawn && CandidateExtension->GetOwner() == CandidatePawn
				&& UGameFrameworkComponentManager::GetForActor(OldPawn) == Manager
				&& UGameFrameworkComponentManager::GetForActor(CandidatePawn) == Manager
				&& OldExtension->GetInitState() == GGYGOGameplayTags::InitState_Spawned
				&& CandidateExtension->GetInitState() == GGYGOGameplayTags::InitState_Spawned
				&& !OldExtension->GetGGYGOAbilitySystemComponent()
				&& !CandidateExtension->GetGGYGOAbilitySystemComponent());
			bPassed &= Test.TestNull(TEXT("A has no second Pawn-owned ASC"), OldPawn->FindComponentByClass<UGGYGOAbilitySystemComponent>());
			bPassed &= Test.TestNull(TEXT("C has no second Pawn-owned ASC"), CandidatePawn->FindComponentByClass<UGGYGOAbilitySystemComponent>());
			bPassed &= Test.TestFalse(TEXT("living host is not Dying"), ASC->HasMatchingGameplayTag(GGYGOGameplayTags::State_Dying));
			bPassed &= Test.TestFalse(TEXT("living host is not Dead"), ASC->HasMatchingGameplayTag(GGYGOGameplayTags::State_Dead));
			return bPassed;
		}

		FGGYGOCombatantBindingSuccessorSnapshot Capture() const
		{
			FGGYGOCombatantBindingSuccessorSnapshot Snapshot;
			Snapshot.bEndpointsLive = IsValid(Host) && IsValid(ASC)
				&& IsValid(OldPawn) && IsValid(CandidatePawn)
				&& IsValid(OldExtension) && IsValid(CandidateExtension)
				&& !Host->IsActorBeingDestroyed() && !ASC->IsBeingDestroyed()
				&& !OldPawn->IsActorBeingDestroyed() && !CandidatePawn->IsActorBeingDestroyed()
				&& !OldExtension->IsBeingDestroyed() && !CandidateExtension->IsBeingDestroyed()
				&& Host->HasActorBegunPlay() && OldPawn->HasActorBegunPlay() && CandidatePawn->HasActorBegunPlay()
				&& OldExtension->HasBegunPlay() && CandidateExtension->HasBegunPlay();
			if (IsValid(Host)) { Snapshot.HostAvatar = Host->GetAvatarPawn(); }
			if (IsValid(ASC))
			{
				Snapshot.CachedOwner = ASC->GetOwnerActor();
				Snapshot.CachedAvatar = ASC->GetAvatarActor();
				Snapshot.ActorInfoAllocation = ASC->AbilityActorInfo.Get();
				if (Snapshot.ActorInfoAllocation)
				{
					Snapshot.ActorInfoOwner = Snapshot.ActorInfoAllocation->OwnerActor.Get();
					Snapshot.ActorInfoAvatar = Snapshot.ActorInfoAllocation->AvatarActor.Get();
					Snapshot.ActorInfoASC = Snapshot.ActorInfoAllocation->AbilitySystemComponent.Get();
				}
			}
			if (IsValid(OldExtension)) { Snapshot.OldCachedASC = OldExtension->GetGGYGOAbilitySystemComponent(); }
			if (IsValid(CandidateExtension)) { Snapshot.CandidateCachedASC = CandidateExtension->GetGGYGOAbilitySystemComponent(); }
			const FName HandlerName(TEXT("HandleAvatarDestroyed"));
			if (IsValid(Host) && IsValid(OldPawn))
			{
				Snapshot.bOldDestroyedSubscription = OldPawn->OnDestroyed.Contains(Host, HandlerName);
			}
			if (IsValid(Host) && IsValid(CandidatePawn))
			{
				Snapshot.bCandidateDestroyedSubscription = CandidatePawn->OnDestroyed.Contains(Host, HandlerName);
			}
			return Snapshot;
		}

		void StartObserving(FAutomationTestBase& Test, bool bSamePawn);
		void StopObserving();
		~FBindingSuccessorFixture();
	};

	bool CheckCompleteBinding(FAutomationTestBase& Test, const FBindingSuccessorFixture& Fixture,
		const FGGYGOCombatantBindingSuccessorSnapshot& Snapshot, bool bTargetIsOld, const TCHAR* Stage)
	{
		APawn* Target = bTargetIsOld ? Fixture.OldPawn : Fixture.CandidatePawn;
		const FString Prefix = FString(Stage) + TEXT(": ");
		bool bPassed = Test.TestTrue(Prefix + TEXT("all real endpoints remain live"), Snapshot.bEndpointsLive);
		bPassed &= Test.TestEqual(Prefix + TEXT("Host Avatar is the successful target"), Snapshot.HostAvatar, Target);
		bPassed &= Test.TestEqual(Prefix + TEXT("cached ASC Owner remains Host"), Snapshot.CachedOwner, static_cast<AActor*>(Fixture.Host));
		bPassed &= Test.TestEqual(Prefix + TEXT("cached ASC Avatar is the successful target"), Snapshot.CachedAvatar, static_cast<AActor*>(Target));
		bPassed &= Test.TestNotNull(Prefix + TEXT("ActorInfo exists"), Snapshot.ActorInfoAllocation);
		bPassed &= Test.TestEqual(Prefix + TEXT("ActorInfo Owner remains Host"), Snapshot.ActorInfoOwner, static_cast<AActor*>(Fixture.Host));
		bPassed &= Test.TestEqual(Prefix + TEXT("ActorInfo Avatar is the successful target"), Snapshot.ActorInfoAvatar, static_cast<AActor*>(Target));
		bPassed &= Test.TestEqual(Prefix + TEXT("ActorInfo retains the original ASC"), Snapshot.ActorInfoASC, static_cast<UAbilitySystemComponent*>(Fixture.ASC));
		bPassed &= Test.TestEqual(Prefix + TEXT("target Extension retains Host ASC"),
			bTargetIsOld ? Snapshot.OldCachedASC : Snapshot.CandidateCachedASC, Fixture.ASC);
		bPassed &= Test.TestTrue(Prefix + TEXT("Host retains target destruction subscription"),
			bTargetIsOld ? Snapshot.bOldDestroyedSubscription : Snapshot.bCandidateDestroyedSubscription);
		return bPassed;
	}

	FString DescribeSnapshot(const TCHAR* Stage, const FGGYGOCombatantBindingSuccessorSnapshot& Snapshot)
	{
		return FString::Printf(TEXT("%s: Live=%d HostAvatar=%s CachedOwner=%s CachedAvatar=%s InfoOwner=%s InfoAvatar=%s InfoASC=%s ACache=%s CCache=%s ASubscription=%d CSubscription=%d"),
			Stage, Snapshot.bEndpointsLive, *GetPathNameSafe(Snapshot.HostAvatar), *GetPathNameSafe(Snapshot.CachedOwner),
			*GetPathNameSafe(Snapshot.CachedAvatar), *GetPathNameSafe(Snapshot.ActorInfoOwner),
			*GetPathNameSafe(Snapshot.ActorInfoAvatar), *GetPathNameSafe(Snapshot.ActorInfoASC),
			*GetPathNameSafe(Snapshot.OldCachedASC), *GetPathNameSafe(Snapshot.CandidateCachedASC),
			Snapshot.bOldDestroyedSubscription, Snapshot.bCandidateDestroyedSubscription);
	}

	/** Weak delegate target; all fields are observations, never production state. */
	struct FBindingSuccessorProbe
	{
		FBindingSuccessorFixture& Fixture;
		FAutomationTestBase& Test;
		const bool bSamePawn;
		bool bObserving = false;
		bool bTakeoverArmed = false;
		bool bCallbackPreconditionsPassed = false;
		bool bCallbackBindingPassed = false;
		bool bTakeoverCompleted = false;
		int32 OldUninitializedCount = 0;
		int32 CandidateUninitializedCount = 0;
		int32 OldInitializedCount = 0;
		int32 CandidateInitializedCount = 0;
		int32 TakeoverAttemptCount = 0;
		int32 SuccessfulTargetUninitializedCount = 0;
		TArray<FName> Events;
		FGGYGOCombatantBindingSuccessorSnapshot BeforeTakeover;
		FGGYGOCombatantBindingSuccessorSnapshot AfterTakeover;

		FBindingSuccessorProbe(FBindingSuccessorFixture& InFixture, FAutomationTestBase& InTest, bool bInSamePawn)
			: Fixture(InFixture), Test(InTest), bSamePawn(bInSamePawn) {}

		int32 GetTargetInitializedCount() const { return bSamePawn ? OldInitializedCount : CandidateInitializedCount; }

		void HandleOldLocalNotice(const FGGYGOPawnASCLocalNotice& Notice)
		{
			if (Notice.Kind == EGGYGOPawnASCLocalNoticeKind::Ready) { HandleOldInitialized(); return; }
			if (Notice.Kind != EGGYGOPawnASCLocalNoticeKind::Released) { return; }
			if (!bObserving) { return; }
			++OldUninitializedCount;
			Events.Add(FName(TEXT("OldUninitialized")));
			if (bSamePawn && bTakeoverCompleted) { ++SuccessfulTargetUninitializedCount; }
			if (!bTakeoverArmed) { return; }
			bTakeoverArmed = false; // Consume before any public call; never recursively request another takeover.
			BeforeTakeover = Fixture.Capture();
			bCallbackPreconditionsPassed = Test.TestTrue(TEXT("callback precondition: live endpoints, old local/ASC Avatar withdrawn, Owner retained, target not yet bound"),
				BeforeTakeover.bEndpointsLive
				&& BeforeTakeover.CachedOwner == Fixture.Host && !BeforeTakeover.CachedAvatar
				&& BeforeTakeover.ActorInfoAllocation && BeforeTakeover.ActorInfoOwner == Fixture.Host
				&& !BeforeTakeover.ActorInfoAvatar && BeforeTakeover.ActorInfoASC == Fixture.ASC
				&& !BeforeTakeover.OldCachedASC && !BeforeTakeover.CandidateCachedASC
				&& !BeforeTakeover.bCandidateDestroyedSubscription);
			Test.AddInfo(DescribeSnapshot(TEXT("Uninitialized.BeforeTakeover"), BeforeTakeover));
			if (!bCallbackPreconditionsPassed) { return; }

			++TakeoverAttemptCount;
			Events.Add(FName(TEXT("TakeoverAttempt")));
			Fixture.Host->AttachAvatar(bSamePawn ? Fixture.OldPawn : Fixture.CandidatePawn);
			AfterTakeover = Fixture.Capture();
			bCallbackBindingPassed = CheckCompleteBinding(Test, Fixture, AfterTakeover, bSamePawn, TEXT("callback successful takeover precondition"));
			bCallbackBindingPassed &= Test.TestEqual(TEXT("callback successful takeover initialized exactly once"), GetTargetInitializedCount(), 1);
			bCallbackBindingPassed &= Test.TestEqual(TEXT("callback keeps original ActorInfo allocation"),
				AfterTakeover.ActorInfoAllocation, BeforeTakeover.ActorInfoAllocation);
			Test.AddInfo(DescribeSnapshot(TEXT("Uninitialized.AfterTakeover"), AfterTakeover));
			bTakeoverCompleted = bCallbackBindingPassed;
			if (bTakeoverCompleted) { Events.Add(FName(TEXT("TakeoverSucceeded"))); }
		}

		void HandleCandidateLocalNotice(const FGGYGOPawnASCLocalNotice& Notice)
		{
			if (Notice.Kind == EGGYGOPawnASCLocalNoticeKind::Ready) { HandleCandidateInitialized(); return; }
			if (Notice.Kind != EGGYGOPawnASCLocalNoticeKind::Released) { return; }
			if (!bObserving) { return; }
			++CandidateUninitializedCount;
			Events.Add(FName(TEXT("CandidateUninitialized")));
			if (!bSamePawn && bTakeoverCompleted) { ++SuccessfulTargetUninitializedCount; }
		}

		void HandleOldInitialized()
		{
			if (bObserving) { ++OldInitializedCount; Events.Add(FName(TEXT("OldInitialized"))); }
		}

		void HandleCandidateInitialized()
		{
			if (bObserving) { ++CandidateInitializedCount; Events.Add(FName(TEXT("CandidateInitialized"))); }
		}
	};

	void FBindingSuccessorFixture::StartObserving(FAutomationTestBase& Test, bool bSamePawn)
	{
		Probe = MakeShared<FBindingSuccessorProbe>(*this, Test, bSamePawn);
		OldNoticeHandle = OldExtension->RegisterLocalAbilitySystemNoticeAndCall(
			FGGYGOPawnASCLocalNoticeDelegate::FDelegate::CreateSP(Probe.ToSharedRef(), &FBindingSuccessorProbe::HandleOldLocalNotice));
		CandidateNoticeHandle = CandidateExtension->RegisterLocalAbilitySystemNoticeAndCall(
			FGGYGOPawnASCLocalNoticeDelegate::FDelegate::CreateSP(Probe.ToSharedRef(), &FBindingSuccessorProbe::HandleCandidateLocalNotice));
		// Registration priming precedes the diagnostic action and is not a takeover notification.
		Probe->bObserving = true;
		Probe->bTakeoverArmed = true;
	}

	void FBindingSuccessorFixture::StopObserving()
	{
		if (Probe)
		{
			Probe->bObserving = false;
			Probe->bTakeoverArmed = false;
		}
		if (IsValid(OldExtension)) { OldExtension->UnregisterLocalAbilitySystemNotice(OldNoticeHandle); }
		if (IsValid(CandidateExtension)) { CandidateExtension->UnregisterLocalAbilitySystemNotice(CandidateNoticeHandle); }
		OldNoticeHandle.Reset();
		CandidateNoticeHandle.Reset();
		Probe.Reset();
	}

	FBindingSuccessorFixture::~FBindingSuccessorFixture()
	{
		StopObserving();
		if (World)
		{
			// Keep GI/Manager alive while only this fixture's actors run real EndPlay.
			if (IsValid(OldPawn)) { OldPawn->Destroy(); }
			if (IsValid(CandidatePawn)) { CandidatePawn->Destroy(); }
			if (IsValid(Host)) { Host->Destroy(); }
			World->EndPlay(EEndPlayReason::Quit);
		}
		if (bGameInstanceInitialized && GameInstance.IsValid()) { GameInstance->Shutdown(); }
		if (World)
		{
			World->DestroyWorld(false);
			Engine->DestroyWorldContext(World);
			if (UPackage* Package = World->GetPackage()) { Package->SetDirtyFlag(false); }
		}
		FNetDelegates::OnReceivedNetworkEncryptionToken = SavedEncryptionToken;
		FNetDelegates::OnReceivedNetworkEncryptionAck = SavedEncryptionAck;
		FNetDelegates::OnReceivedNetworkEncryptionFailure = SavedEncryptionFailure;
		GameInstance.Reset();
	}

	bool RunBindingSuccessorDiagnostic(FAutomationTestBase& Test, bool bSamePawn)
	{
		FBindingSuccessorFixture Fixture;
		if (!Fixture.Initialize(Test)) { return false; }
		Fixture.Host->AttachAvatar(Fixture.OldPawn);
		const FGGYGOCombatantBindingSuccessorSnapshot Initial = Fixture.Capture();
		bool bInitialPassed = CheckCompleteBinding(Test, Fixture, Initial, true, TEXT("initial public Attach(A)"));
		bInitialPassed &= Test.TestNull(TEXT("initial C Extension is unbound"), Initial.CandidateCachedASC);
		bInitialPassed &= Test.TestFalse(TEXT("initial C has no Host destruction subscription"), Initial.bCandidateDestroyedSubscription);
		if (!bInitialPassed) { return false; }

		Fixture.StartObserving(Test, bSamePawn);
		Fixture.Host->DetachAvatar(Fixture.OldPawn);
		const FGGYGOCombatantBindingSuccessorSnapshot Final = Fixture.Capture();
		Test.AddInfo(DescribeSnapshot(TEXT("Detach.Returned"), Final));
		Test.AddInfo(FString::Printf(TEXT("Observed counts: AUninitialized=%d CUninitialized=%d AInitialized=%d CInitialized=%d Attempts=%d TargetUninitializedAfterSuccess=%d"),
			Fixture.Probe->OldUninitializedCount, Fixture.Probe->CandidateUninitializedCount,
			Fixture.Probe->OldInitializedCount, Fixture.Probe->CandidateInitializedCount,
			Fixture.Probe->TakeoverAttemptCount, Fixture.Probe->SuccessfulTargetUninitializedCount));
		if (!Fixture.Probe->bCallbackPreconditionsPassed || !Fixture.Probe->bCallbackBindingPassed)
		{
			Test.AddInfo(TEXT("Successful public callback takeover was not established; final target assertions are not evidence of the old-tail defect."));
			return false;
		}

		bool bPassed = CheckCompleteBinding(Test, Fixture, Final, bSamePawn, TEXT("old Detach tail must preserve successful successor"));
		bPassed &= Test.TestEqual(TEXT("final binding keeps the successfully adopted ActorInfo allocation"),
			Final.ActorInfoAllocation, Fixture.Probe->AfterTakeover.ActorInfoAllocation);
		bPassed &= Test.TestEqual(TEXT("one actual old Uninitialized notification"), Fixture.Probe->OldUninitializedCount, 1);
		bPassed &= Test.TestEqual(TEXT("one armed public takeover attempt"), Fixture.Probe->TakeoverAttemptCount, 1);
		bPassed &= Test.TestEqual(TEXT("one successful target Initialized notification"), Fixture.Probe->GetTargetInitializedCount(), 1);
		bPassed &= Test.TestEqual(TEXT("old tail never uninitializes the successful target"), Fixture.Probe->SuccessfulTargetUninitializedCount, 0);
		bPassed &= Test.TestEqual(TEXT("C receives no uninitialization after callback takeover"), Fixture.Probe->CandidateUninitializedCount, 0);
		if (bSamePawn)
		{
			bPassed &= Test.TestNull(TEXT("unused C remains unbound"), Final.CandidateCachedASC);
			bPassed &= Test.TestFalse(TEXT("unused C has no Host subscription"), Final.bCandidateDestroyedSubscription);
			bPassed &= Test.TestEqual(TEXT("unused C never initializes"), Fixture.Probe->CandidateInitializedCount, 0);
		}
		else
		{
			bPassed &= Test.TestNull(TEXT("old A cache remains withdrawn"), Final.OldCachedASC);
			bPassed &= Test.TestFalse(TEXT("old A subscription is removed"), Final.bOldDestroyedSubscription);
			bPassed &= Test.TestEqual(TEXT("old A never reinitializes"), Fixture.Probe->OldInitializedCount, 0);
		}
		bPassed &= Test.TestEqual(TEXT("only original notification, attempt, target ready and success events"),
			Fixture.Probe->Events.Num(), 4);
		if (Fixture.Probe->Events.Num() >= 4)
		{
			bPassed &= Test.TestEqual(TEXT("event 1 is old Uninitialized"), Fixture.Probe->Events[0], FName(TEXT("OldUninitialized")));
			bPassed &= Test.TestEqual(TEXT("event 2 is unique takeover attempt"), Fixture.Probe->Events[1], FName(TEXT("TakeoverAttempt")));
			bPassed &= Test.TestEqual(TEXT("event 3 is target Initialized"), Fixture.Probe->Events[2],
				FName(bSamePawn ? TEXT("OldInitialized") : TEXT("CandidateInitialized")));
			bPassed &= Test.TestEqual(TEXT("event 4 is callback success"), Fixture.Probe->Events[3], FName(TEXT("TakeoverSucceeded")));
		}
		return bPassed;
	}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FGGYGOCombatantBindingSuccessorDiagnostic,
	"ProjectDiagnostics.Combatants.BindingSuccessor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FGGYGOCombatantBindingSuccessorDiagnostic::GetTests(TArray<FString>& OutBeautifiedNames,
	TArray<FString>& OutTestCommands) const
{
	if (FParse::Param(FCommandLine::Get(), SuccessorDiagnosticFlag))
	{
		OutBeautifiedNames.Add(DifferentPawnCase);
		OutTestCommands.Add(DifferentPawnCase);
		OutBeautifiedNames.Add(SamePawnCase);
		OutTestCommands.Add(SamePawnCase);
	}
}

bool FGGYGOCombatantBindingSuccessorDiagnostic::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("explicit binding successor diagnostic launch flag"),
		FParse::Param(FCommandLine::Get(), SuccessorDiagnosticFlag)))
	{
		return false;
	}
	if (!TestTrue(TEXT("exact supported binding successor diagnostic case"),
		Parameters == DifferentPawnCase || Parameters == SamePawnCase))
	{
		return false;
	}
	return RunBindingSuccessorDiagnostic(*this, Parameters == SamePawnCase);
}
#endif // WITH_DEV_AUTOMATION_TESTS

#if WITH_DEV_AUTOMATION_TESTS

#include "Character/Components/GGYGOPawnExtensionComponent.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

namespace GGYGOPawnExtensionLocalResourcesTests
{
	bool IsEmptyContext(const FGGYGOAvatarBindingContext& Context)
	{
		return Context.Binding.Serial == 0 && Context.Binding.Issuer.IsExplicitlyNull()
			&& Context.LastActorInfoWrite.Serial == 0
			&& Context.LastActorInfoWrite.Issuer.IsExplicitlyNull();
	}

	bool ExpectLocalResult(FAutomationTestBase& Test, const TCHAR* Stage,
		const FGGYGOPawnASCLocalResult& Result, EGGYGOPawnASCLocalOutcome Outcome,
		EGGYGOPawnASCLocalReason Reason, const FGGYGOPawnASCResourceHandle& Resource,
		bool bChanged)
	{
		bool bPassed = Test.TestEqual(FString::Printf(TEXT("%s Outcome"), Stage),
			static_cast<uint8>(Result.Outcome), static_cast<uint8>(Outcome));
		bPassed &= Test.TestEqual(FString::Printf(TEXT("%s Reason"), Stage),
			static_cast<uint8>(Result.Reason), static_cast<uint8>(Reason));
		bPassed &= Test.TestTrue(FString::Printf(TEXT("%s exact original resource"), Stage),
			Result.Resource.HasSameResource(Resource));
		bPassed &= Test.TestEqual(FString::Printf(TEXT("%s historical local change"), Stage),
			Result.bLocalChanged, bChanged);
		return bPassed;
	}

	bool ExpectCommittedSuccess(FAutomationTestBase& Test, const TCHAR* Stage,
		const FGGYGOAvatarBindingResult& Result)
	{
		bool bPassed = Test.TestEqual(FString::Printf(TEXT("%s Outcome"), Stage),
			static_cast<uint8>(Result.Outcome), static_cast<uint8>(EGGYGOAvatarBindingOutcome::Succeeded));
		bPassed &= Test.TestEqual(FString::Printf(TEXT("%s Reason"), Stage),
			static_cast<uint8>(Result.Reason), static_cast<uint8>(EGGYGOAvatarBindingReason::None));
		bPassed &= Test.TestTrue(FString::Printf(TEXT("%s real commit"), Stage), Result.bCommitted);
		bPassed &= Test.TestTrue(FString::Printf(TEXT("%s issued Context"), Stage),
			Result.CommittedContext.HasIssuedContext());
		return bPassed;
	}

	// Use the existing native transaction tests' isolated, pre-BeginPlay World pattern.
	// No GI/Host/ability probe is needed to exercise this local resource contract.
	struct FFixture
	{
		UEngine* Engine = nullptr;
		UWorld* World = nullptr;
		AActor* Owner = nullptr;
		APawn* Pawn = nullptr;
		UGGYGOAbilitySystemComponent* ASC = nullptr;
		UGGYGOPawnExtensionComponent* Extension = nullptr;
		const FGameplayAbilityActorInfo* OriginalActorInfo = nullptr;
		bool bCallerScopeOpen = true;

		~FFixture()
		{
			bCallerScopeOpen = false;
			if (World)
			{
				World->DestroyWorld(false);
				if (Engine) { Engine->DestroyWorldContext(World); }
				if (UPackage* WorldPackage = World->GetPackage())
				{
					// Only the newly created test package; never clear other packages' dirty flags.
					WorldPackage->SetDirtyFlag(false);
				}
			}
		}

		bool Initialize(FAutomationTestBase& Test)
		{
			Engine = GEngine;
			if (!Test.TestNotNull(TEXT("Real engine"), Engine)) { return false; }
			UWorld::InitializationValues Init;
			Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(false)
				.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Init);
			if (!Test.TestNotNull(TEXT("Isolated real Game World"), World)) { return false; }
			Engine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			Owner = World->SpawnActor<AActor>();
			Pawn = World->SpawnActor<APawn>();
			if (!Test.TestNotNull(TEXT("Original ASC Owner"), Owner)
				|| !Test.TestNotNull(TEXT("Original Pawn"), Pawn)) { return false; }
			ASC = NewObject<UGGYGOAbilitySystemComponent>(Owner);
			if (!Test.TestNotNull(TEXT("Real project ASC"), ASC)) { return false; }
			ASC->RegisterComponent();
			Extension = NewObject<UGGYGOPawnExtensionComponent>(Pawn);
			if (!Test.TestNotNull(TEXT("Real PawnExtension"), Extension)) { return false; }
			Pawn->AddInstanceComponent(Extension);
			Extension->RegisterComponent();
			OriginalActorInfo = ASC->AbilityActorInfo.Get();
			TArray<UGGYGOPawnExtensionComponent*> Extensions;
			Pawn->GetComponents<UGGYGOPawnExtensionComponent>(Extensions);
			bool bPassed = Test.TestTrue(TEXT("ASC registered on original Owner"),
				ASC->IsRegistered() && ASC->GetOwner() == Owner);
			bPassed &= Test.TestTrue(TEXT("Extension registered on original Pawn"),
				Extension->IsRegistered() && Extension->GetOwner() == Pawn);
			bPassed &= Test.TestTrue(TEXT("Exactly one production Extension"),
				Extensions.Num() == 1 && Extensions[0] == Extension);
			bPassed &= Test.TestNotNull(TEXT("Original native ActorInfo allocation"), OriginalActorInfo);
			bPassed &= Test.TestTrue(TEXT("Real authority Owner and Pawn"), Owner->HasAuthority() && Pawn->HasAuthority());
			bPassed &= Test.TestTrue(TEXT("Empty initial ASC Context"), IsEmptyContext(ASC->GetAvatarBindingContext()));
			bPassed &= Test.TestFalse(TEXT("Empty initial local slot"), Extension->GetCurrentLocalAbilitySystemResource().HasResource());
			bPassed &= Test.TestFalse(TEXT("No BeginPlay coverage claimed"), World->HasBegunPlay());
			return bPassed;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPawnExtensionLocalResourcesLifecycleTest,
	"GGYGO.Character.PawnExtension.LocalResources.LifecycleAndSameContextReinstall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPawnExtensionLocalResourcesLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace GGYGOPawnExtensionLocalResourcesTests;
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	UGGYGOAbilitySystemComponent* const ASC = Fixture.ASC;
	UGGYGOPawnExtensionComponent* const Extension = Fixture.Extension;
	FGGYGOAvatarBindingContext Context;
	FGGYGOAvatarBindingPublicationReceipt Receipt;
	FGGYGOPawnASCResourceHandle H1;
	FGGYGOPawnASCResourceHandle H2;
	FDelegateHandle ASCNoticeHandle;
	TArray<FDelegateHandle> LocalNoticeHandles;
	int32 ASCNoticeCount = 0;
	int32 EarlyReadyCount = 0;
	int32 EarlyReleasedCount = 0;
	int32 ReplayReadyCount = 0;
	int32 ReplayReleasedCount = 0;
	int32 UnreadyReadyCount = 0;
	int32 UnreadyReleasedCount = 0;
	bool bBridgeArmed = true;
	bool bOriginalReleaseArmed = true;
	bool bCallbackChecksPassed = true;
	bool bCleanupPerformed = false;
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(ASC);
	const TWeakObjectPtr<AActor> OriginalOwner(Fixture.Owner);
	const TWeakObjectPtr<APawn> OriginalPawn(Fixture.Pawn);
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> OriginalExtension(Extension);
	const auto IsOriginalCallerCurrent = [Scope = &Fixture.bCallerScopeOpen,
		OriginalASC, OriginalOwner, OriginalPawn]()
	{
		return *Scope && OriginalASC.IsValid() && OriginalOwner.IsValid() && OriginalPawn.IsValid()
			&& !OriginalASC->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& !OriginalOwner->IsActorBeingDestroyed() && !OriginalPawn->IsActorBeingDestroyed()
			&& OriginalASC->GetOwner() == OriginalOwner.Get();
	};
	const auto CheckBoundState = [&](const TCHAR* Stage, bool bExpectedLocalReady)
	{
		const FGameplayAbilityActorInfo* Info = ASC->AbilityActorInfo.Get();
		if (!TestNotNull(FString::Printf(TEXT("%s live ActorInfo"), Stage), Info)) { return false; }
		EGGYGOAvatarBindingReason Reason;
		bool bPassed = TestTrue(FString::Printf(TEXT("%s original ActorInfo allocation"), Stage),
			Info == Fixture.OriginalActorInfo);
		bPassed &= TestTrue(FString::Printf(TEXT("%s cached and native Owner"), Stage),
			ASC->GetOwnerActor() == Fixture.Owner && Info->OwnerActor.Get() == Fixture.Owner);
		bPassed &= TestTrue(FString::Printf(TEXT("%s cached and native Avatar"), Stage),
			ASC->GetAvatarActor() == Fixture.Pawn && Info->AvatarActor.Get() == Fixture.Pawn);
		bPassed &= TestTrue(FString::Printf(TEXT("%s exact Context unchanged"), Stage),
			ASC->GetAvatarBindingContext().HasSameContext(Context));
		bPassed &= TestTrue(FString::Printf(TEXT("%s real Context valid"), Stage),
			ASC->CheckAvatarBindingContext(Context, Reason) == EGGYGOAvatarBindingOutcome::Succeeded);
		bPassed &= TestFalse(FString::Printf(TEXT("%s native Busy closed"), Stage), ASC->IsAvatarBindingNativeWriteBusy());
		bPassed &= TestEqual(FString::Printf(TEXT("%s Getter matches explicit local Ready phase"), Stage),
			Extension->GetGGYGOAbilitySystemComponent(), bExpectedLocalReady ? ASC : nullptr);
		return bPassed;
	};
	const auto CheckLocalSlot = [&](const TCHAR* Stage, const FGGYGOPawnASCResourceHandle& Resource, bool bReady)
	{
		bool bPassed = TestTrue(FString::Printf(TEXT("%s exact raw slot"), Stage),
			Extension->GetCurrentLocalAbilitySystemResource().HasSameResource(Resource));
		bPassed &= TestTrue(FString::Printf(TEXT("%s Installed"), Stage), Extension->IsLocalAbilitySystemResourceInstalled(Resource));
		bPassed &= TestEqual(FString::Printf(TEXT("%s Ready"), Stage), Extension->IsLocalAbilitySystemResourceReady(Resource), bReady);
		if (Resource.HasSameResource(H2))
		{
			bPassed &= TestTrue(FString::Printf(TEXT("%s H1 history retained"), Stage), H1.HasResource());
			bPassed &= TestFalse(FString::Printf(TEXT("%s H1 remains withdrawn"), Stage), Extension->IsLocalAbilitySystemResourceInstalled(H1));
			bPassed &= TestFalse(FString::Printf(TEXT("%s H1 remains unready"), Stage), Extension->IsLocalAbilitySystemResourceReady(H1));
		}
		bPassed &= CheckBoundState(Stage, bReady);
		return bPassed;
	};
	const auto CheckOriginalNotice = [&](const TCHAR* Stage, const FGGYGOPawnASCLocalNotice& Notice)
	{
		bool bPassed = TestTrue(FString::Printf(TEXT("%s original H1"), Stage), Notice.Resource.HasSameResource(H1));
		if (Notice.Kind == EGGYGOPawnASCLocalNoticeKind::Ready)
		{
			bPassed &= TestTrue(FString::Printf(TEXT("%s original PublishedContext"), Stage), Notice.PublishedContext.HasSameContext(Context));
			bPassed &= TestTrue(FString::Printf(TEXT("%s real Dispatching"), Stage), ASC->IsAvatarBindingNoticeDispatching(Receipt));
			bPassed &= TestTrue(FString::Printf(TEXT("%s real local Ready"), Stage), Extension->IsLocalAbilitySystemResourceReady(H1));
		}
		else if (Notice.Kind == EGGYGOPawnASCLocalNoticeKind::Released)
		{
			bPassed &= TestTrue(FString::Printf(TEXT("%s empty Released Context"), Stage), IsEmptyContext(Notice.PublishedContext));
		}
		else
		{
			AddError(FString::Printf(TEXT("%s unexpected local notice Kind=%u"), Stage, static_cast<uint32>(Notice.Kind)));
			bPassed = false;
		}
		bPassed &= CheckBoundState(Stage, Notice.Kind == EGGYGOPawnASCLocalNoticeKind::Ready);
		return bPassed;
	};
	const auto RemoveOriginalSubscriptions = [&]()
	{
		for (FDelegateHandle Handle : LocalNoticeHandles)
		{
			Extension->UnregisterLocalAbilitySystemNotice(Handle);
		}
		LocalNoticeHandles.Reset();
		if (ASCNoticeHandle.IsValid())
		{
			ASC->OnAvatarBindingNotice().Remove(ASCNoticeHandle);
			ASCNoticeHandle.Reset();
		}
	};
	const auto Cleanup = [&]()
	{
		bCleanupPerformed = true;
		RemoveOriginalSubscriptions();
		bool bPassed = true;
		// Release the saved successor first: the original notification then has no live successor.
		// These are retained handles, never resources adopted from a later current-slot query.
		for (const FGGYGOPawnASCResourceHandle& Resource : {H2, H1})
		{
			if (!Resource.HasResource()) { continue; }
			const FGGYGOPawnASCLocalResult Withdraw = Extension->WithdrawLocalAbilitySystemResources(Resource);
			bPassed &= TestTrue(TEXT("Cleanup Withdraw exact original resource"),
				Withdraw.Outcome == EGGYGOPawnASCLocalOutcome::Succeeded
				&& Withdraw.Reason == EGGYGOPawnASCLocalReason::None && Withdraw.Resource.HasSameResource(Resource));
			const FGGYGOPawnASCLocalResult Released = Extension->NotifyLocalResourcesReleased(Resource);
			bPassed &= TestTrue(TEXT("Cleanup Released exact original resource"),
				Released.Outcome == EGGYGOPawnASCLocalOutcome::Succeeded
				&& Released.Reason == EGGYGOPawnASCLocalReason::None && Released.Resource.HasSameResource(Resource));
		}
		bPassed &= TestFalse(TEXT("Cleanup leaves local slot empty"), Extension->GetCurrentLocalAbilitySystemResource().HasResource());
		if (!Context.HasIssuedContext()) { return bPassed; }
		EGGYGOAvatarBindingReason Reason;
		if (!TestTrue(TEXT("Cleanup retains exact original Context; never adopts a successor"),
			ASC->CheckAvatarBindingContext(Context, Reason) == EGGYGOAvatarBindingOutcome::Succeeded)) { return false; }
		FGGYGOAvatarBindingRequest ClearRequest;
		ClearRequest.Kind = EGGYGOAvatarBindingKind::Clear;
		ClearRequest.ExpectedContext = Context;
		ClearRequest.ClearMode = EGGYGOAvatarBindingClearMode::PreserveOwner;
		ClearRequest.IsRequestContextCurrent = IsOriginalCallerCurrent;
		FGGYGOAvatarBindingPublicationReceipt ClearReceipt;
		const FGGYGOAvatarBindingResult Clear = ASC->TryExecuteAvatarActorInfoTransaction(ClearRequest, ClearReceipt);
		if (!ExpectCommittedSuccess(*this, TEXT("Cleanup native Clear"), Clear)) { return false; }
		const FGameplayAbilityActorInfo* Info = ASC->AbilityActorInfo.Get();
		if (!TestNotNull(TEXT("Cleanup ActorInfo after Clear"), Info)) { return false; }
		bPassed &= TestTrue(TEXT("Clear preserves original Owner and allocation"),
			Info == Fixture.OriginalActorInfo && Info->OwnerActor.Get() == Fixture.Owner && ASC->GetOwnerActor() == Fixture.Owner);
		bPassed &= TestTrue(TEXT("Clear removes cached and native Avatar"),
			ASC->GetAvatarActor() == nullptr && Info->AvatarActor.IsExplicitlyNull());
		bPassed &= TestTrue(TEXT("Clear committed exact expected Before"), Clear.Before.HasSameContext(Context));
		bPassed &= TestTrue(TEXT("Clear committed Context is actually current"),
			ASC->GetAvatarBindingContext().HasSameContext(Clear.CommittedContext));
		if (!bPassed) { return false; }
		const auto ClearPublishedScope = [IsOriginalCallerCurrent, OriginalASC, OriginalOwner,
			ClearContext = Clear.CommittedContext]()
		{
			EGGYGOAvatarBindingReason QueryReason;
			return IsOriginalCallerCurrent() && OriginalASC->GetOwnerActor() == OriginalOwner.Get()
				&& OriginalASC->GetAvatarActor() == nullptr
				&& OriginalASC->CheckAvatarBindingContext(ClearContext, QueryReason) == EGGYGOAvatarBindingOutcome::Succeeded;
		};
		const FGGYGOAvatarBindingResult Published = ASC->PublishAvatarBindingNotice(ClearReceipt, ClearPublishedScope);
		bPassed &= ExpectCommittedSuccess(*this, TEXT("Cleanup real Clear Publish"), Published);
		const FGameplayAbilityActorInfo* PublishedInfo = ASC->AbilityActorInfo.Get();
		if (!TestNotNull(TEXT("Cleanup ActorInfo after Clear Publish"), PublishedInfo)) { return false; }
		bPassed &= TestTrue(TEXT("Clear Publish preserves original Owner/allocation and cleared Avatar"),
			PublishedInfo == Fixture.OriginalActorInfo && PublishedInfo->OwnerActor.Get() == Fixture.Owner
			&& ASC->GetOwnerActor() == Fixture.Owner && ASC->GetAvatarActor() == nullptr
			&& PublishedInfo->AvatarActor.IsExplicitlyNull());
		bPassed &= TestTrue(TEXT("Clear Publish retains its exact Context"),
			Published.CommittedContext.HasSameContext(Clear.CommittedContext)
			&& ASC->GetAvatarBindingContext().HasSameContext(Clear.CommittedContext));
		bPassed &= TestTrue(TEXT("Clear Publish Context remains actually valid"),
			ASC->CheckAvatarBindingContext(Clear.CommittedContext, Reason) == EGGYGOAvatarBindingOutcome::Succeeded);
		bPassed &= TestFalse(TEXT("Clear Publish leaves native Busy closed"), ASC->IsAvatarBindingNativeWriteBusy());
		bPassed &= TestFalse(TEXT("Clear Receipt is no longer Dispatching"), ASC->IsAvatarBindingNoticeDispatching(ClearReceipt));
		return bPassed;
	};
	// This guard dies before every borrowed callback capture; Fixture/World dies last.
	ON_SCOPE_EXIT { if (!bCleanupPerformed) { Cleanup(); } };

	FGGYGOAvatarBindingRequest InitRequest;
	InitRequest.Kind = EGGYGOAvatarBindingKind::Init;
	InitRequest.OwnerActor = Fixture.Owner;
	InitRequest.AvatarActor = Fixture.Pawn;
	InitRequest.IsRequestContextCurrent = IsOriginalCallerCurrent;
	const FGGYGOAvatarBindingResult Commit = ASC->TryBootstrapAvatarActorInfoTransaction(InitRequest, Receipt);
	if (Commit.bCommitted) { Context = Commit.CommittedContext; }
	if (!ExpectCommittedSuccess(*this, TEXT("Bootstrap"), Commit) || !CheckBoundState(TEXT("Bootstrap"), false)) { return false; }

	const FGGYGOPawnASCLocalResult Install = Extension->InstallLocalAbilitySystemResources(ASC, Fixture.Pawn, Context);
	H1 = Install.Resource;
	if (!ExpectLocalResult(*this, TEXT("Install H1"), Install, EGGYGOPawnASCLocalOutcome::Succeeded,
		EGGYGOPawnASCLocalReason::None, H1, true)
		|| !TestTrue(TEXT("H1 exact original ASC/Pawn/Binding identity"),
			H1.GetIdentity().ASC.HasSameIndexAndSerialNumber(OriginalASC)
			&& H1.GetIdentity().Pawn.HasSameIndexAndSerialNumber(OriginalPawn)
			&& H1.GetIdentity().Binding.HasSameIdentity(Context.Binding))
		|| !CheckBoundState(TEXT("Install H1"), false)) { return false; }
	if (!CheckLocalSlot(TEXT("H1 visible before Publish"), H1, false)) { return false; }
	const FGGYGOPawnASCLocalResult DuplicateInstall = Extension->InstallLocalAbilitySystemResources(ASC, Fixture.Pawn, Context);
	if (!ExpectLocalResult(*this, TEXT("Duplicate Install H1"), DuplicateInstall, EGGYGOPawnASCLocalOutcome::Succeeded,
		EGGYGOPawnASCLocalReason::None, H1, false) || !CheckLocalSlot(TEXT("Duplicate Install H1"), H1, false)) { return false; }

	const FDelegateHandle EarlyHandle = Extension->RegisterLocalAbilitySystemNoticeAndCall(
		FGGYGOPawnASCLocalNoticeDelegate::FDelegate::CreateLambda([&](const FGGYGOPawnASCLocalNotice& Notice)
		{
			if (!CheckOriginalNotice(TEXT("Early observer"), Notice)) { bCallbackChecksPassed = false; return; }
			if (Notice.Kind == EGGYGOPawnASCLocalNoticeKind::Ready) { ++EarlyReadyCount; return; }
			++EarlyReleasedCount;
			if (!TestTrue(TEXT("Original Released callback runs once"), bOriginalReleaseArmed)
				|| !CheckLocalSlot(TEXT("H2 before old callback"), H2, false)) { bCallbackChecksPassed = false; return; }
			bOriginalReleaseArmed = false;
			const FGGYGOPawnASCLocalResult OldCleanup = Extension->WithdrawLocalAbilitySystemResources(Notice.Resource);
			bCallbackChecksPassed &= ExpectLocalResult(*this, TEXT("Old callback exact Withdraw H1"), OldCleanup,
				EGGYGOPawnASCLocalOutcome::Succeeded, EGGYGOPawnASCLocalReason::None, H1, false);
			if (!bCallbackChecksPassed) { return; }
			bCallbackChecksPassed &= CheckLocalSlot(TEXT("H2 after old callback"), H2, false);
		}));
	LocalNoticeHandles.Add(EarlyHandle);
	if (!TestTrue(TEXT("Exact early registration token"), EarlyHandle.IsValid())
		|| !bCallbackChecksPassed
		|| !TestEqual(TEXT("Unready H1 gives no registration replay"), EarlyReadyCount, 0)
		|| !TestEqual(TEXT("No premature Released"), EarlyReleasedCount, 0)
		|| !CheckBoundState(TEXT("Early registration"), false)) { return false; }
	const FGGYGOPawnASCLocalResult PendingReady = Extension->NotifyLocalResourcesReady(H1, Receipt);
	if (!ExpectLocalResult(*this, TEXT("Pending Receipt cannot establish Ready"), PendingReady,
		EGGYGOPawnASCLocalOutcome::Rejected, EGGYGOPawnASCLocalReason::PublicationNotDispatching, H1, false)
		|| !bCallbackChecksPassed
		|| !TestEqual(TEXT("Pending rejection emits no Ready"), EarlyReadyCount, 0)
		|| !CheckLocalSlot(TEXT("H1 remains unready after Pending refusal"), H1, false)) { return false; }

	ASCNoticeHandle = ASC->OnAvatarBindingNotice().AddLambda(
		[&](const FGGYGOAvatarBindingPublicationReceipt& DeliveredReceipt, const FGGYGOAvatarBindingNotice& Notice)
		{
			++ASCNoticeCount;
			bool bPassed = TestTrue(TEXT("Real Init bridge once"), bBridgeArmed);
			bPassed &= TestTrue(TEXT("Real ASC Initialized notice"), Notice.Kind == EGGYGOAvatarBindingNoticeKind::Initialized);
			bPassed &= TestTrue(TEXT("Delivered original committed Context"), Notice.After.HasSameContext(Context));
			bPassed &= TestTrue(TEXT("Delivered original Owner/Avatar"), Notice.OwnerActor.Get() == Fixture.Owner && Notice.AvatarActor.Get() == Fixture.Pawn);
			bPassed &= TestTrue(TEXT("Delivered proof really Dispatching"), ASC->IsAvatarBindingNoticeDispatching(DeliveredReceipt));
			bPassed &= CheckBoundState(TEXT("Real ASC dispatch"), false);
			if (!bPassed) { bCallbackChecksPassed = false; return; }
			bBridgeArmed = false;
			const FGGYGOPawnASCLocalResult Ready = Extension->NotifyLocalResourcesReady(H1, DeliveredReceipt);
			if (!ExpectLocalResult(*this, TEXT("Real Ready H1"), Ready, EGGYGOPawnASCLocalOutcome::Succeeded,
				EGGYGOPawnASCLocalReason::None, H1, true) || !bCallbackChecksPassed
				|| !CheckLocalSlot(TEXT("H1 Ready inside Dispatching"), H1, true)) { bCallbackChecksPassed = false; return; }
			const FGGYGOPawnASCLocalResult Duplicate = Extension->NotifyLocalResourcesReady(H1, DeliveredReceipt);
			if (!ExpectLocalResult(*this, TEXT("Duplicate Ready inside Dispatching"), Duplicate,
				EGGYGOPawnASCLocalOutcome::Succeeded, EGGYGOPawnASCLocalReason::None, H1, false)
				|| !bCallbackChecksPassed
				|| !TestEqual(TEXT("Exactly one early Ready"), EarlyReadyCount, 1)
				|| !CheckLocalSlot(TEXT("H1 after duplicate Ready"), H1, true)) { bCallbackChecksPassed = false; return; }
			const FDelegateHandle ReplayHandle = Extension->RegisterLocalAbilitySystemNoticeAndCall(
				FGGYGOPawnASCLocalNoticeDelegate::FDelegate::CreateLambda([&](const FGGYGOPawnASCLocalNotice& LocalNotice)
				{
					if (!CheckOriginalNotice(TEXT("Replay observer"), LocalNotice)) { bCallbackChecksPassed = false; return; }
					if (LocalNotice.Kind == EGGYGOPawnASCLocalNoticeKind::Ready) { ++ReplayReadyCount; }
					else { ++ReplayReleasedCount; }
				}));
			LocalNoticeHandles.Add(ReplayHandle);
			bCallbackChecksPassed &= TestTrue(TEXT("Exact late replay registration token"), ReplayHandle.IsValid());
			bCallbackChecksPassed &= TestEqual(TEXT("Late registration replays exact Ready once"), ReplayReadyCount, 1);
			bCallbackChecksPassed &= CheckLocalSlot(TEXT("H1 after exact replay"), H1, true);
		});
	if (!TestTrue(TEXT("Exact real ASC subscription token"), ASCNoticeHandle.IsValid())) { return false; }
	const auto IsOriginalPublicationCurrent = [IsOriginalCallerCurrent, OriginalExtension, OriginalPawn, OriginalH1 = H1]()
	{
		return IsOriginalCallerCurrent() && OriginalExtension.IsValid()
			&& OriginalExtension->GetOwner() == OriginalPawn.Get()
			&& OriginalExtension->IsLocalAbilitySystemResourceInstalled(OriginalH1);
	};
	const FGGYGOAvatarBindingResult Published = ASC->PublishAvatarBindingNotice(Receipt, IsOriginalPublicationCurrent);
	if (!ExpectCommittedSuccess(*this, TEXT("Real Init Publish"), Published) || !bCallbackChecksPassed
		|| !TestEqual(TEXT("Exactly one actual ASC notice"), ASCNoticeCount, 1)
		|| !TestTrue(TEXT("Publish retains original commit"), Published.CommittedContext.HasSameContext(Context))
		|| !TestFalse(TEXT("Original Receipt dispatch finished"), ASC->IsAvatarBindingNoticeDispatching(Receipt))
		|| !CheckLocalSlot(TEXT("H1 Ready after consumed Publish"), H1, true)) { return false; }

	const FGGYGOPawnASCLocalResult WithdrawH1 = Extension->WithdrawLocalAbilitySystemResources(H1);
	if (!ExpectLocalResult(*this, TEXT("Withdraw H1"), WithdrawH1, EGGYGOPawnASCLocalOutcome::Succeeded,
		EGGYGOPawnASCLocalReason::None, H1, true)
		|| !TestFalse(TEXT("Withdraw clears original slot"), Extension->GetCurrentLocalAbilitySystemResource().HasResource())
		|| !TestFalse(TEXT("Withdrawn H1 not Installed"), Extension->IsLocalAbilitySystemResourceInstalled(H1))
		|| !TestFalse(TEXT("Withdrawn H1 not Ready"), Extension->IsLocalAbilitySystemResourceReady(H1))
		|| !CheckBoundState(TEXT("Withdraw H1"), false)) { return false; }
	// Deliberately outside the completed Publish: no policy assertion about an active R authorizing H2.
	const FGGYGOPawnASCLocalResult Reinstall = Extension->InstallLocalAbilitySystemResources(ASC, Fixture.Pawn, Context);
	H2 = Reinstall.Resource;
	if (!ExpectLocalResult(*this, TEXT("Same Context Install H2"), Reinstall, EGGYGOPawnASCLocalOutcome::Succeeded,
		EGGYGOPawnASCLocalReason::None, H2, true)
		|| !TestTrue(TEXT("H1/H2 retain same identity triple"), H1.GetIdentity().HasSameIdentity(H2.GetIdentity()))
		|| !TestFalse(TEXT("H1/H2 are different opaque records"), H1.HasSameResource(H2))
		|| !CheckLocalSlot(TEXT("Same Context H2 installed, not Ready"), H2, false)) { return false; }
	const FDelegateHandle UnreadyHandle = Extension->RegisterLocalAbilitySystemNoticeAndCall(
		FGGYGOPawnASCLocalNoticeDelegate::FDelegate::CreateLambda([&](const FGGYGOPawnASCLocalNotice& Notice)
		{
			if (!CheckOriginalNotice(TEXT("Unready registration observer"), Notice)) { bCallbackChecksPassed = false; return; }
			if (Notice.Kind == EGGYGOPawnASCLocalNoticeKind::Ready) { ++UnreadyReadyCount; }
			else { ++UnreadyReleasedCount; }
		}));
	LocalNoticeHandles.Add(UnreadyHandle);
	if (!TestTrue(TEXT("Exact unready registration token"), UnreadyHandle.IsValid())
		|| !TestEqual(TEXT("H2 installation gives no Ready replay"), UnreadyReadyCount, 0)
		|| !bCallbackChecksPassed || !CheckLocalSlot(TEXT("H2 after no replay"), H2, false)) { return false; }
	const FGGYGOPawnASCLocalResult HistoricalReady = Extension->NotifyLocalResourcesReady(H2, Receipt);
	if (!ExpectLocalResult(*this, TEXT("Consumed historical Receipt refused for H2"), HistoricalReady,
		EGGYGOPawnASCLocalOutcome::Rejected, EGGYGOPawnASCLocalReason::PublicationNotDispatching, H2, false)
		|| !bCallbackChecksPassed
		|| !CheckLocalSlot(TEXT("H2 after historical refusal"), H2, false)) { return false; }
	const FGGYGOPawnASCLocalResult ReleasedH1 = Extension->NotifyLocalResourcesReleased(H1);
	if (!ExpectLocalResult(*this, TEXT("Original Released preserves successor"), ReleasedH1,
		EGGYGOPawnASCLocalOutcome::Stale, EGGYGOPawnASCLocalReason::CallbackInvalidated, H1, true)
		|| !bCallbackChecksPassed || !CheckLocalSlot(TEXT("H2 after original Released"), H2, false)) { return false; }
	const FGGYGOPawnASCLocalResult DuplicateWithdraw = Extension->WithdrawLocalAbilitySystemResources(H1);
	if (!ExpectLocalResult(*this, TEXT("Duplicate original Withdraw"), DuplicateWithdraw,
		EGGYGOPawnASCLocalOutcome::Succeeded, EGGYGOPawnASCLocalReason::None, H1, false)
		|| !bCallbackChecksPassed
		|| !CheckLocalSlot(TEXT("H2 after duplicate Withdraw"), H2, false)) { return false; }
	const FGGYGOPawnASCLocalResult DuplicateReleased = Extension->NotifyLocalResourcesReleased(H1);
	if (!ExpectLocalResult(*this, TEXT("Duplicate original Released"), DuplicateReleased,
		EGGYGOPawnASCLocalOutcome::Succeeded, EGGYGOPawnASCLocalReason::None, H1, false)
		|| !bCallbackChecksPassed
		|| !CheckLocalSlot(TEXT("H2 after duplicate Released"), H2, false)
		|| !TestEqual(TEXT("Early Ready stays exactly one"), EarlyReadyCount, 1)
		|| !TestEqual(TEXT("Replay Ready stays exactly one"), ReplayReadyCount, 1)
		|| !TestEqual(TEXT("Unready registration never receives Ready"), UnreadyReadyCount, 0)
		|| !TestEqual(TEXT("Early exact Released once"), EarlyReleasedCount, 1)
		|| !TestEqual(TEXT("Replay observer exact Released once"), ReplayReleasedCount, 1)
		|| !TestEqual(TEXT("Unready observer exact Released once"), UnreadyReleasedCount, 1)) { return false; }

	RemoveOriginalSubscriptions();
	const FGGYGOPawnASCLocalResult WithdrawH2 = Extension->WithdrawLocalAbilitySystemResources(H2);
	if (!ExpectLocalResult(*this, TEXT("Final exact Withdraw H2"), WithdrawH2, EGGYGOPawnASCLocalOutcome::Succeeded,
		EGGYGOPawnASCLocalReason::None, H2, true) || !bCallbackChecksPassed
		|| !TestFalse(TEXT("Final Withdraw H2 clears slot"), Extension->GetCurrentLocalAbilitySystemResource().HasResource())
		|| !TestFalse(TEXT("Final Withdraw H2 closes Installed"), Extension->IsLocalAbilitySystemResourceInstalled(H2))
		|| !TestFalse(TEXT("Final Withdraw H2 closes Ready"), Extension->IsLocalAbilitySystemResourceReady(H2))
		|| !CheckBoundState(TEXT("Final Withdraw H2"), false)) { return false; }
	const FGGYGOPawnASCLocalResult ReleasedH2 = Extension->NotifyLocalResourcesReleased(H2);
	if (!ExpectLocalResult(*this, TEXT("Final exact Released H2"), ReleasedH2, EGGYGOPawnASCLocalOutcome::Succeeded,
		EGGYGOPawnASCLocalReason::None, H2, true) || !bCallbackChecksPassed || !CheckBoundState(TEXT("Final Released H2"), false)
		|| !TestFalse(TEXT("Final local slot empty"), Extension->GetCurrentLocalAbilitySystemResource().HasResource())
		|| !TestEqual(TEXT("Original early subscription returned"), EarlyReleasedCount, 1)
		|| !TestEqual(TEXT("Original replay subscription returned"), ReplayReleasedCount, 1)
		|| !TestEqual(TEXT("Original unready subscription returned"), UnreadyReleasedCount, 1)) { return false; }
	const bool bCleanupPassed = Cleanup();
	return bCleanupPassed && bCallbackChecksPassed
		&& TestEqual(TEXT("Original ASC subscription returned before Clear Publish"), ASCNoticeCount, 1);
}

#endif // WITH_DEV_AUTOMATION_TESTS

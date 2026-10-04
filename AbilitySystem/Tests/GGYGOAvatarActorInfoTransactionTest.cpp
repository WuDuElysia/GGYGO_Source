#include "AbilitySystem/Tests/GGYGOAvatarActorInfoTransactionTestTypes.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameplayAbilitySpec.h"
#include "AbilitySystemGlobals.h"
#include "GameplayCueManager.h"
#include "GameplayCueSet.h"
#include "GameplayTagsManager.h"
#include "NativeGameplayTags.h"
#include "UObject/Package.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAvatarActorInfoTransactionTestTypes)

UGGYGOAvatarActorInfoTransactionTestAbility::UGGYGOAvatarActorInfoTransactionTestAbility(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
}

void UGGYGOAvatarActorInfoTransactionTestAbility::ArmNativeAvatarHookForTest(
	FGGYGOAvatarActorInfoNativeHook InHook)
{
	NativeAvatarHook = MoveTemp(InHook);
}

void UGGYGOAvatarActorInfoTransactionTestAbility::DisarmNativeAvatarHookForTest()
{
	NativeAvatarHook.Unbind();
}

void UGGYGOAvatarActorInfoTransactionTestAbility::OnAvatarSet(
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec)
{
	Super::OnAvatarSet(ActorInfo, Spec);
	if (NativeAvatarHook.IsBound())
	{
		FGGYGOAvatarActorInfoNativeHook OneShot = MoveTemp(NativeAvatarHook);
		NativeAvatarHook.Unbind();
		++ArmedNativeCalls;
		OneShot.Execute(ActorInfo, Spec);
	}
}

UGGYGOAvatarBindingPublicationTestAbility::UGGYGOAvatarBindingPublicationTestAbility(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UGGYGOAvatarBindingPublicationTestAbility::ArmPawnNoticeHookForTest(
	FGGYGOAvatarBindingPublicationPawnHook InHook)
{
	PawnNoticeHook = MoveTemp(InHook);
}

void UGGYGOAvatarBindingPublicationTestAbility::DisarmPawnNoticeHookForTest()
{
	PawnNoticeHook.Unbind();
}

void UGGYGOAvatarBindingPublicationTestAbility::OnPawnAvatarSet()
{
	++PawnNoticeCalls;
	LastNotifiedAvatar = GetAvatarActorFromActorInfo();
	Super::OnPawnAvatarSet();
	if (PawnNoticeHook.IsBound())
	{
		FGGYGOAvatarBindingPublicationPawnHook OneShot = MoveTemp(PawnNoticeHook);
		PawnNoticeHook.Unbind();
		OneShot.Execute();
	}
}

UGGYGOAvatarBindingCancelTestAbility::UGGYGOAvatarBindingCancelTestAbility(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
}

UGGYGOAvatarBindingCueRemovalTestNotify::UGGYGOAvatarBindingCueRemovalTestNotify(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UGGYGOAvatarBindingCueRemovalTestNotify::ArmRemovedHookForTest(FGGYGOAvatarBindingCueRemovalHook InHook)
{
	RemovedHook = MoveTemp(InHook);
}

void UGGYGOAvatarBindingCueRemovalTestNotify::DisarmRemovedHookForTest()
{
	RemovedHook.Unbind();
}

bool UGGYGOAvatarBindingCueRemovalTestNotify::WhileActive_Implementation(
	AActor* MyTarget, const FGameplayCueParameters& Parameters) const
{
	++WhileActiveCalls;
	LastWhileActiveTarget = MyTarget;
	return true;
}

bool UGGYGOAvatarBindingCueRemovalTestNotify::OnRemove_Implementation(
	AActor* MyTarget, const FGameplayCueParameters& Parameters) const
{
	++RemovedCalls;
	LastRemovedTarget = MyTarget;
	if (RemovedHook.IsBound())
	{
		FGGYGOAvatarBindingCueRemovalHook OneShot = MoveTemp(RemovedHook);
		RemovedHook.Unbind();
		OneShot.Execute(MyTarget, Parameters);
	}
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_GGYGO_ActorInfoTransaction_CueRemovalProbe,
	"GameplayCue.GGYGO.Tests.ActorInfoTransaction.CueRemovalProbe");

namespace GGYGOAvatarActorInfoTransactionTests
{
	bool IsEmptyContext(const FGGYGOAvatarBindingContext& Context)
	{
		return Context.Binding.Serial == 0 && Context.Binding.Issuer.IsExplicitlyNull()
			&& Context.LastActorInfoWrite.Serial == 0
			&& Context.LastActorInfoWrite.Issuer.IsExplicitlyNull();
	}

	// History comparisons also cover the deliberately empty Bootstrap Before value.
	bool SameContextValue(const FGGYGOAvatarBindingContext& Left,
		const FGGYGOAvatarBindingContext& Right)
	{
		return Left.Binding.Serial == Right.Binding.Serial
			&& Left.Binding.Issuer.HasSameIndexAndSerialNumber(Right.Binding.Issuer)
			&& Left.LastActorInfoWrite.Serial == Right.LastActorInfoWrite.Serial
			&& Left.LastActorInfoWrite.Issuer.HasSameIndexAndSerialNumber(Right.LastActorInfoWrite.Issuer);
	}

	struct FFixture
	{
		UWorld* World = nullptr;
		UEngine* Engine = nullptr;
		AActor* Owner = nullptr;
		AActor* AvatarA = nullptr;
		AActor* AvatarB = nullptr;
		AActor* AvatarC = nullptr;
		UGGYGOAbilitySystemComponent* ASC = nullptr;
		TWeakObjectPtr<UGGYGOAvatarActorInfoTransactionTestAbility> Probe;
		FGameplayAbilitySpecHandle ProbeHandle;
		bool bCallerScopeOpen = true;

		~FFixture()
		{
			bCallerScopeOpen = false;
			if (UGGYGOAvatarActorInfoTransactionTestAbility* Instance = Probe.Get())
			{
				Instance->DisarmNativeAvatarHookForTest();
			}
			if (IsValid(ASC) && ProbeHandle.IsValid())
			{
				ASC->ClearAbility(ProbeHandle);
			}
			Probe.Reset();
			if (World)
			{
				// The world context must survive component and actor teardown.
				World->DestroyWorld(false);
				if (Engine) { Engine->DestroyWorldContext(World); }
				if (UPackage* WorldPackage = World->GetPackage())
				{
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
			if (!Test.TestNotNull(TEXT("Isolated real world"), World)) { return false; }
			Engine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			Owner = World->SpawnActor<AActor>();
			AvatarA = World->SpawnActor<AActor>();
			AvatarB = World->SpawnActor<AActor>();
			AvatarC = World->SpawnActor<AActor>();
			if (!Test.TestNotNull(TEXT("Owner"), Owner)
				|| !Test.TestNotNull(TEXT("Avatar A"), AvatarA)
				|| !Test.TestNotNull(TEXT("Avatar B"), AvatarB)
				|| !Test.TestNotNull(TEXT("Avatar C"), AvatarC)) { return false; }
			ASC = NewObject<UGGYGOAbilitySystemComponent>(Owner);
			if (!Test.TestNotNull(TEXT("Real project ASC"), ASC)) { return false; }
			ASC->RegisterComponent();
			return Test.TestTrue(TEXT("Native ActorInfo allocation exists"), ASC->AbilityActorInfo.IsValid())
				&& Test.TestTrue(TEXT("No issued binding before Bootstrap"), IsEmptyContext(ASC->GetAvatarBindingContext()))
				&& Test.TestFalse(TEXT("World has not begun play"), World->HasBegunPlay());
		}

		FGGYGOAvatarBindingRequest Request(EGGYGOAvatarBindingKind Kind,
			const FGGYGOAvatarBindingContext& Expected, AActor* NewAvatar = nullptr,
			EGGYGOAvatarBindingClearMode ClearMode = EGGYGOAvatarBindingClearMode::None) const
		{
			FGGYGOAvatarBindingRequest Value;
			Value.Kind = Kind;
			Value.ExpectedContext = Expected;
			Value.ClearMode = ClearMode;
			if (Kind == EGGYGOAvatarBindingKind::Init)
			{
				Value.OwnerActor = Owner;
				Value.AvatarActor = NewAvatar;
			}
			const bool* ScopeOpen = &bCallerScopeOpen;
			const TWeakObjectPtr<UGGYGOAbilitySystemComponent> WeakASC(ASC);
			const TWeakObjectPtr<AActor> WeakOwner(Owner);
			Value.IsRequestContextCurrent = [ScopeOpen, WeakASC, WeakOwner]()
			{
				const AActor* LiveOwner = WeakOwner.Get();
				return *ScopeOpen && WeakASC.IsValid() && LiveOwner && !LiveOwner->IsActorBeingDestroyed();
			};
			return Value;
		}

		bool GrantProbe(FAutomationTestBase& Test)
		{
			ProbeHandle = ASC->GiveAbility(FGameplayAbilitySpec(
				UGGYGOAvatarActorInfoTransactionTestAbility::StaticClass(), 1));
			FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(ProbeHandle);
			Probe = Spec ? Cast<UGGYGOAvatarActorInfoTransactionTestAbility>(Spec->GetPrimaryInstance()) : nullptr;
			UGGYGOAvatarActorInfoTransactionTestAbility* Instance = Probe.Get();
			if (!Test.TestTrue(TEXT("Real GiveAbility handle"), ProbeHandle.IsValid())
				|| !Test.TestNotNull(TEXT("Real primary instance"), Instance)) { return false; }
			return Test.TestTrue(TEXT("Instance is not the CDO"), Instance->IsInstantiated())
				&& Test.TestEqual(TEXT("Native per-actor instancing"), Instance->GetInstancingPolicy(),
					EGameplayAbilityInstancingPolicy::InstancedPerActor)
				&& Test.TestEqual(TEXT("GiveAbility occurred before arming"), Instance->GetArmedNativeCallsForTest(), 0);
		}
	};

	bool CheckActual(FAutomationTestBase& Test, const FFixture& Fixture,
		AActor* ExpectedOwner, AActor* ExpectedAvatar, USkeletalMeshComponent* ExpectedMesh = nullptr)
	{
		const FGameplayAbilityActorInfo* Actual = Fixture.ASC->AbilityActorInfo.Get();
		if (!Test.TestNotNull(TEXT("Real ActorInfo remains allocated"), Actual)) { return false; }
		bool bOK = Test.TestTrue(TEXT("ActorInfo uses original ASC"), Actual->AbilitySystemComponent.Get() == Fixture.ASC);
		bOK &= Test.TestTrue(TEXT("Actual ActorInfo Owner"), Actual->OwnerActor.Get() == ExpectedOwner);
		bOK &= Test.TestTrue(TEXT("Actual ActorInfo Avatar"), Actual->AvatarActor.Get() == ExpectedAvatar);
		bOK &= Test.TestTrue(TEXT("Native cached Owner"), Fixture.ASC->GetOwnerActor() == ExpectedOwner);
		bOK &= Test.TestTrue(TEXT("Native cached Avatar"), Fixture.ASC->GetAvatarActor_Direct() == ExpectedAvatar);
		bOK &= Test.TestTrue(TEXT("Actor-only fixture has no controller"), Actual->PlayerController.IsExplicitlyNull());
		bOK &= Test.TestTrue(TEXT("Actual ActorInfo mesh cache"),
			Actual->SkeletalMeshComponent.HasSameIndexAndSerialNumber(TWeakObjectPtr<USkeletalMeshComponent>(ExpectedMesh)));
		bOK &= Test.TestTrue(TEXT("Actor-only fixture has no movement"), Actual->MovementComponent.IsExplicitlyNull());
		bOK &= Test.TestNull(TEXT("Actor-only fixture has no animation instance"), Actual->GetAnimInstance());
		return bOK;
	}

	bool CheckHistory(FAutomationTestBase& Test, const FGGYGOAvatarBindingPublicationReceipt& Publication,
		const FGGYGOAvatarBindingResult& Expected, EGGYGOAvatarBindingNoticeKind ExpectedKind,
		AActor* ExpectedOwner, AActor* ExpectedAvatar)
	{
		FGGYGOAvatarBindingResult ReadResult;
		FGGYGOAvatarBindingNotice Notice;
		if (!Test.TestTrue(TEXT("Receipt has immutable committed history"),
			Publication.TryGetCommittedEvidence(ReadResult, Notice))) { return false; }
		bool bOK = Test.TestEqual(TEXT("Historical result outcome"), ReadResult.Outcome, Expected.Outcome);
		bOK &= Test.TestEqual(TEXT("Historical result reason"), ReadResult.Reason, Expected.Reason);
		bOK &= Test.TestEqual(TEXT("Historical committed flag"), ReadResult.bCommitted, Expected.bCommitted);
		bOK &= Test.TestTrue(TEXT("Historical exact operation"), ReadResult.Operation.HasSameIdentity(Expected.Operation));
		bOK &= Test.TestTrue(TEXT("Historical exact Before"), SameContextValue(ReadResult.Before, Expected.Before));
		bOK &= Test.TestTrue(TEXT("Historical exact commit"), SameContextValue(ReadResult.CommittedContext, Expected.CommittedContext));
		bOK &= Test.TestEqual(TEXT("Notice preserves original operation kind"), Notice.Kind, ExpectedKind);
		bOK &= Test.TestTrue(TEXT("Notice exact operation"), Notice.Operation.HasSameIdentity(Expected.Operation));
		bOK &= Test.TestTrue(TEXT("Notice exact Before"), SameContextValue(Notice.Before, Expected.Before));
		bOK &= Test.TestTrue(TEXT("Notice exact After"), SameContextValue(Notice.After, Expected.CommittedContext));
		bOK &= Test.TestTrue(TEXT("Notice exact weak Owner"), Notice.OwnerActor.HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(ExpectedOwner)));
		bOK &= Test.TestTrue(TEXT("Notice exact weak Avatar"), Notice.AvatarActor.HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(ExpectedAvatar)));
		return bOK;
	}

	bool CheckCommit(FAutomationTestBase& Test, const FFixture& Fixture,
		const FGGYGOAvatarBindingResult& Result, const FGGYGOAvatarBindingPublicationReceipt& Publication,
		const FGGYGOAvatarBindingContext& Before, EGGYGOAvatarBindingNoticeKind Kind,
		AActor* NoticeOwner, AActor* NoticeAvatar, bool bPreserveBinding = false)
	{
		if (!Test.TestEqual(TEXT("Transaction succeeded"), Result.Outcome, EGGYGOAvatarBindingOutcome::Succeeded)
			|| !Test.TestEqual(TEXT("Transaction reason None"), Result.Reason, EGGYGOAvatarBindingReason::None)
			|| !Test.TestTrue(TEXT("Transaction really committed"), Result.bCommitted)) { return false; }
		bool bOK = Test.TestTrue(TEXT("Operation is ASC-issued"), Result.Operation.HasIssuedIdentity()
			&& Result.Operation.Issuer.Get() == Fixture.ASC);
		bOK &= Test.TestTrue(TEXT("Result retains request Before"), SameContextValue(Result.Before, Before));
		bOK &= Test.TestTrue(TEXT("Committed context is current"), Result.CommittedContext.HasSameContext(Fixture.ASC->GetAvatarBindingContext()));
		bOK &= Test.TestTrue(TEXT("LastWrite is this operation"), Result.CommittedContext.LastActorInfoWrite.HasSameIdentity(Result.Operation));
		bOK &= Test.TestTrue(TEXT("LastWrite advances"), Result.Operation.Serial > Before.LastActorInfoWrite.Serial);
		if (bPreserveBinding)
		{
			bOK &= Test.TestTrue(TEXT("Refresh preserves exact Binding"), Result.CommittedContext.Binding.HasSameIdentity(Before.Binding));
		}
		else
		{
			bOK &= Test.TestTrue(TEXT("Init/Clear create Binding from this operation"),
				Result.CommittedContext.Binding.Serial == Result.Operation.Serial
				&& Result.CommittedContext.Binding.Issuer.HasSameIndexAndSerialNumber(Result.Operation.Issuer));
			bOK &= Test.TestFalse(TEXT("Init/Clear replace previous Binding"), Result.CommittedContext.Binding.HasSameIdentity(Before.Binding));
		}
		EGGYGOAvatarBindingReason Reason;
		bOK &= Test.TestEqual(TEXT("Committed actual context validates"),
			Fixture.ASC->CheckAvatarBindingContext(Result.CommittedContext, Reason), EGGYGOAvatarBindingOutcome::Succeeded);
		bOK &= Test.TestEqual(TEXT("Actual validation reason None"), Reason, EGGYGOAvatarBindingReason::None);
		bOK &= Test.TestFalse(TEXT("Native window closes before return"), Fixture.ASC->IsAvatarBindingNativeWriteBusy());
		bOK &= CheckHistory(Test, Publication, Result, Kind, NoticeOwner, NoticeAvatar);
		return bOK;
	}

	bool CheckEmptyPublication(FAutomationTestBase& Test,
		const FGGYGOAvatarBindingPublicationReceipt& Empty,
		const FGGYGOAvatarBindingPublicationReceipt& Seed)
	{
		FGGYGOAvatarBindingResult Result;
		FGGYGOAvatarBindingNotice Notice;
		if (!Test.TestTrue(TEXT("Reset check starts with genuine nonempty history"),
			Seed.TryGetCommittedEvidence(Result, Notice))) { return false; }
		bool bOK = Test.TestFalse(TEXT("Failure has no publication proof"), Empty.TryGetCommittedEvidence(Result, Notice));
		bOK &= Test.TestEqual(TEXT("Read resets Outcome"), Result.Outcome, EGGYGOAvatarBindingOutcome::Rejected);
		bOK &= Test.TestEqual(TEXT("Read resets Reason"), Result.Reason, EGGYGOAvatarBindingReason::InvalidRequest);
		bOK &= Test.TestFalse(TEXT("Read resets committed flag"), Result.bCommitted);
		bOK &= Test.TestTrue(TEXT("Read resets operation"), Result.Operation.Serial == 0 && Result.Operation.Issuer.IsExplicitlyNull());
		bOK &= Test.TestTrue(TEXT("Read resets result contexts"), IsEmptyContext(Result.Before) && IsEmptyContext(Result.CommittedContext));
		bOK &= Test.TestEqual(TEXT("Read resets notice kind"), Notice.Kind, EGGYGOAvatarBindingNoticeKind::Invalid);
		bOK &= Test.TestTrue(TEXT("Read resets notice operation"), Notice.Operation.Serial == 0 && Notice.Operation.Issuer.IsExplicitlyNull());
		bOK &= Test.TestTrue(TEXT("Read resets notice contexts"), IsEmptyContext(Notice.Before) && IsEmptyContext(Notice.After));
		bOK &= Test.TestTrue(TEXT("Read resets notice endpoints"), Notice.OwnerActor.IsExplicitlyNull() && Notice.AvatarActor.IsExplicitlyNull());
		return bOK;
	}

	bool CheckFailure(FAutomationTestBase& Test, const FGGYGOAvatarBindingResult& Result,
		EGGYGOAvatarBindingOutcome Outcome, EGGYGOAvatarBindingReason Reason, bool bReserved)
	{
		bool bOK = Test.TestEqual(TEXT("Exact failed outcome"), Result.Outcome, Outcome);
		bOK &= Test.TestEqual(TEXT("Exact failed reason"), Result.Reason, Reason);
		bOK &= Test.TestFalse(TEXT("Failure did not commit"), Result.bCommitted);
		bOK &= Test.TestTrue(TEXT("Failure has no committed context"), IsEmptyContext(Result.CommittedContext));
		bOK &= Test.TestEqual(TEXT("Failure operation issuance"), Result.Operation.HasIssuedIdentity(), bReserved);
		if (!bReserved)
		{
			bOK &= Test.TestTrue(TEXT("Rejected admission issued nothing"),
				Result.Operation.Serial == 0 && Result.Operation.Issuer.IsExplicitlyNull() && IsEmptyContext(Result.Before));
		}
		return bOK;
	}

	struct FNativeObservation
	{
		int32 Calls = 0;
		bool bOriginalASC = false;
		bool bOriginalSpec = false;
		bool bActualAvatarWritten = false;
		bool bBusy = false;
		bool bBusyAfterNested = false;
		bool bCallerBefore = false;
		bool bCallerAfter = false;
		bool bInvalidated = false;
		EGGYGOAvatarBindingReason InvalidationReason = EGGYGOAvatarBindingReason::InvalidRequest;
		FGGYGOAvatarBindingResult NestedResult;
		FGGYGOAvatarBindingPublicationReceipt NestedPublication;

		void Observe(const FFixture& Fixture, const FGameplayAbilityActorInfo* ActorInfo,
			const FGameplayAbilitySpec& Spec, AActor* ExpectedAvatar)
		{
			++Calls;
			bOriginalASC = ActorInfo && ActorInfo == Fixture.ASC->AbilityActorInfo.Get()
				&& ActorInfo->AbilitySystemComponent.Get() == Fixture.ASC;
			bOriginalSpec = Spec.Handle == Fixture.ProbeHandle && Spec.GetPrimaryInstance() == Fixture.Probe.Get();
			bActualAvatarWritten = ActorInfo && ActorInfo->OwnerActor.Get() == Fixture.Owner
				&& ActorInfo->AvatarActor.Get() == ExpectedAvatar
				&& Fixture.ASC->GetAvatarActor_Direct() == ExpectedAvatar;
			bBusy = Fixture.ASC->IsAvatarBindingNativeWriteBusy();
		}

		bool CheckCallback(FAutomationTestBase& Test, const FFixture& Fixture) const
		{
			const UGGYGOAvatarActorInfoTransactionTestAbility* Instance = Fixture.Probe.Get();
			if (!Test.TestNotNull(TEXT("Original native probe remains alive"), Instance)) { return false; }
			bool bOK = Test.TestEqual(TEXT("One real armed callback"), Calls, 1);
			bOK &= Test.TestTrue(TEXT("Native callback uses original ASC and allocation"), bOriginalASC);
			bOK &= Test.TestTrue(TEXT("Native callback uses real granted Spec/instance"), bOriginalSpec);
			bOK &= Test.TestTrue(TEXT("Native callback observes actual new Avatar"), bActualAvatarWritten);
			bOK &= Test.TestTrue(TEXT("Callback is inside real native Busy window"), bBusy);
			bOK &= Test.TestEqual(TEXT("Instance hook is single use"), Instance->GetArmedNativeCallsForTest(), 1);
			return bOK;
		}
	};

	bool Bootstrap(FAutomationTestBase& Test, FFixture& Fixture,
		FGGYGOAvatarBindingResult& Result, FGGYGOAvatarBindingPublicationReceipt& Publication)
	{
		const FGGYGOAvatarBindingRequest Request = Fixture.Request(EGGYGOAvatarBindingKind::Init, {}, Fixture.AvatarA);
		Result = Fixture.ASC->TryBootstrapAvatarActorInfoTransaction(Request, Publication);
		return CheckCommit(Test, Fixture, Result, Publication, {}, EGGYGOAvatarBindingNoticeKind::Initialized,
				Fixture.Owner, Fixture.AvatarA) && CheckActual(Test, Fixture, Fixture.Owner, Fixture.AvatarA);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAvatarActorInfoNativeLifecycleAndHistory,
	"GGYGO.AbilitySystem.ActorInfoTransaction.NativeLifecycleAndHistory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAvatarActorInfoNativeLifecycleAndHistory::RunTest(const FString& Parameters)
{
	using namespace GGYGOAvatarActorInfoTransactionTests;
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	FGGYGOAvatarBindingPublicationReceipt FirstPublication;
	const FGGYGOAvatarBindingRequest BootstrapRequest = Fixture.Request(EGGYGOAvatarBindingKind::Init, {});
	const FGGYGOAvatarBindingResult First = Fixture.ASC->TryBootstrapAvatarActorInfoTransaction(BootstrapRequest, FirstPublication);
	if (!CheckCommit(*this, Fixture, First, FirstPublication, {}, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, nullptr)
		|| !CheckActual(*this, Fixture, Fixture.Owner, nullptr)) { return false; }

	FGGYGOAvatarBindingPublicationReceipt InitPublication;
	const FGGYGOAvatarBindingRequest InitRequest = Fixture.Request(EGGYGOAvatarBindingKind::Init, First.CommittedContext, Fixture.AvatarA);
	const FGGYGOAvatarBindingResult Init = Fixture.ASC->TryExecuteAvatarActorInfoTransaction(InitRequest, InitPublication);
	if (!CheckCommit(*this, Fixture, Init, InitPublication, First.CommittedContext, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, Fixture.AvatarA)
		|| !CheckActual(*this, Fixture, Fixture.Owner, Fixture.AvatarA)) { return false; }

	// A real component appears after Init. Only native Refresh discovers it in ActorInfo.
	USkeletalMeshComponent* RefreshMesh = NewObject<USkeletalMeshComponent>(Fixture.AvatarA);
	if (!TestNotNull(TEXT("Transient mesh for native Refresh"), RefreshMesh)) { return false; }
	Fixture.AvatarA->AddInstanceComponent(RefreshMesh);
	RefreshMesh->RegisterComponent();
	if (!TestTrue(TEXT("New mesh is registered"), RefreshMesh->IsRegistered())
		|| !TestTrue(TEXT("ActorInfo cache precedes new component"), Fixture.ASC->AbilityActorInfo->SkeletalMeshComponent.IsExplicitlyNull())) { return false; }
	FGGYGOAvatarBindingPublicationReceipt RefreshPublication;
	const FGGYGOAvatarBindingRequest RefreshRequest = Fixture.Request(EGGYGOAvatarBindingKind::Refresh, Init.CommittedContext);
	const FGGYGOAvatarBindingResult Refresh = Fixture.ASC->TryExecuteAvatarActorInfoTransaction(RefreshRequest, RefreshPublication);
	if (!CheckCommit(*this, Fixture, Refresh, RefreshPublication, Init.CommittedContext, EGGYGOAvatarBindingNoticeKind::Refreshed, Fixture.Owner, Fixture.AvatarA, true)
		|| !CheckActual(*this, Fixture, Fixture.Owner, Fixture.AvatarA, RefreshMesh)) { return false; }

	FGGYGOAvatarBindingPublicationReceipt PreservePublication;
	const FGGYGOAvatarBindingRequest PreserveRequest = Fixture.Request(EGGYGOAvatarBindingKind::Clear, Refresh.CommittedContext,
		nullptr, EGGYGOAvatarBindingClearMode::PreserveOwner);
	const FGGYGOAvatarBindingResult Preserve = Fixture.ASC->TryExecuteAvatarActorInfoTransaction(PreserveRequest, PreservePublication);
	if (!CheckCommit(*this, Fixture, Preserve, PreservePublication, Refresh.CommittedContext, EGGYGOAvatarBindingNoticeKind::Released, Fixture.Owner, Fixture.AvatarA)
		|| !CheckActual(*this, Fixture, Fixture.Owner, nullptr)) { return false; }

	FGGYGOAvatarBindingPublicationReceipt OwnerOnlyPublication;
	const FGGYGOAvatarBindingRequest OwnerOnlyRequest = Fixture.Request(EGGYGOAvatarBindingKind::Init, Preserve.CommittedContext);
	const FGGYGOAvatarBindingResult OwnerOnly = Fixture.ASC->TryExecuteAvatarActorInfoTransaction(OwnerOnlyRequest, OwnerOnlyPublication);
	if (!CheckCommit(*this, Fixture, OwnerOnly, OwnerOnlyPublication, Preserve.CommittedContext, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, nullptr)
		|| !CheckActual(*this, Fixture, Fixture.Owner, nullptr)) { return false; }

	FGGYGOAvatarBindingPublicationReceipt ClearPublication;
	const FGGYGOAvatarBindingRequest ClearRequest = Fixture.Request(EGGYGOAvatarBindingKind::Clear, OwnerOnly.CommittedContext,
		nullptr, EGGYGOAvatarBindingClearMode::ClearActorInfo);
	const FGGYGOAvatarBindingResult Clear = Fixture.ASC->TryExecuteAvatarActorInfoTransaction(ClearRequest, ClearPublication);
	if (!CheckCommit(*this, Fixture, Clear, ClearPublication, OwnerOnly.CommittedContext, EGGYGOAvatarBindingNoticeKind::Released, Fixture.Owner, nullptr)
		|| !CheckActual(*this, Fixture, nullptr, nullptr)) { return false; }

	FGGYGOAvatarBindingResult MutableHistory;
	FGGYGOAvatarBindingNotice MutableNotice;
	if (!TestTrue(TEXT("History copy available"), FirstPublication.TryGetCommittedEvidence(MutableHistory, MutableNotice))) { return false; }
	MutableHistory = FGGYGOAvatarBindingResult{};
	MutableNotice = FGGYGOAvatarBindingNotice{};
	EGGYGOAvatarBindingReason StaleReason;
	bool bOK = TestEqual(TEXT("Historical context has been superseded"),
		Fixture.ASC->CheckAvatarBindingContext(First.CommittedContext, StaleReason), EGGYGOAvatarBindingOutcome::Stale);
	bOK &= TestEqual(TEXT("Historical context exact mismatch"), StaleReason, EGGYGOAvatarBindingReason::ExpectedContextMismatch);
	bOK &= CheckHistory(*this, FirstPublication, First, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, nullptr);
	bOK &= CheckHistory(*this, InitPublication, Init, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, Fixture.AvatarA);
	bOK &= CheckHistory(*this, PreservePublication, Preserve, EGGYGOAvatarBindingNoticeKind::Released, Fixture.Owner, Fixture.AvatarA);
	return bOK;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAvatarActorInfoRevokedReplacementAndOutputReset,
	"GGYGO.AbilitySystem.ActorInfoTransaction.RevokedReplacementAndOutputReset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAvatarActorInfoRevokedReplacementAndOutputReset::RunTest(const FString& Parameters)
{
	using namespace GGYGOAvatarActorInfoTransactionTests;
	FNativeObservation Observation;
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	FGGYGOAvatarBindingResult First;
	FGGYGOAvatarBindingPublicationReceipt FirstPublication;
	if (!Bootstrap(*this, Fixture, First, FirstPublication) || !Fixture.GrantProbe(*this)) { return false; }
	EGGYGOAvatarBindingReason Reason;
	if (!TestTrue(TEXT("Explicit revocation accepted"), Fixture.ASC->InvalidateAvatarBinding(
			First.CommittedContext, EGGYGOAvatarBindingReason::OperationInvalidated, Reason))
		|| !TestEqual(TEXT("Revocation reason None"), Reason, EGGYGOAvatarBindingReason::None)
		|| !TestTrue(TEXT("Repeated same-context revocation is idempotent"), Fixture.ASC->InvalidateAvatarBinding(
			First.CommittedContext, EGGYGOAvatarBindingReason::OperationInvalidated, Reason))
		|| !TestEqual(TEXT("Repeated revocation reason None"), Reason, EGGYGOAvatarBindingReason::None)) { return false; }
	const FGGYGOAvatarBindingRequest Request = Fixture.Request(EGGYGOAvatarBindingKind::Init, First.CommittedContext, Fixture.AvatarB);
	FGGYGOAvatarBindingPublicationReceipt RejectedPublication = FirstPublication;
	const FGGYGOAvatarBindingResult Rejected = Fixture.ASC->TryExecuteAvatarActorInfoTransaction(Request, RejectedPublication);
	if (!CheckFailure(*this, Rejected, EGGYGOAvatarBindingOutcome::Stale, EGGYGOAvatarBindingReason::OperationInvalidated, false)
		|| !CheckEmptyPublication(*this, RejectedPublication, FirstPublication)
		|| !CheckActual(*this, Fixture, Fixture.Owner, Fixture.AvatarA)) { return false; }

	Fixture.Probe->ArmNativeAvatarHookForTest(FGGYGOAvatarActorInfoNativeHook::CreateLambda(
		[&](const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec)
		{
			Observation.Observe(Fixture, ActorInfo, Spec, Fixture.AvatarB);
			Observation.bCallerBefore = Request.IsRequestContextCurrent();
			Observation.bInvalidated = Fixture.ASC->InvalidateAvatarBinding(
				First.CommittedContext, EGGYGOAvatarBindingReason::OperationInvalidated, Observation.InvalidationReason);
			Observation.bCallerAfter = Request.IsRequestContextCurrent();
		}));
	FGGYGOAvatarBindingPublicationReceipt ReplacementPublication;
	const FGGYGOAvatarBindingResult Replacement = Fixture.ASC->TryReplaceRevokedAvatarActorInfoTransaction(Request, ReplacementPublication);
	bool bOK = Observation.CheckCallback(*this, Fixture);
	bOK &= TestTrue(TEXT("Repeated native callback revocation accepted"), Observation.bInvalidated);
	bOK &= TestEqual(TEXT("Repeated callback revocation reason None"), Observation.InvalidationReason, EGGYGOAvatarBindingReason::None);
	bOK &= TestTrue(TEXT("Caller pure query stays true across idempotent revocation"), Observation.bCallerBefore && Observation.bCallerAfter);
	bOK &= CheckCommit(*this, Fixture, Replacement, ReplacementPublication, First.CommittedContext,
		EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, Fixture.AvatarB);
	bOK &= CheckActual(*this, Fixture, Fixture.Owner, Fixture.AvatarB);
	bOK &= CheckHistory(*this, FirstPublication, First, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, Fixture.AvatarA);
	return bOK;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAvatarActorInfoNativeRevocationBlocksOrdinaryCommit,
	"GGYGO.AbilitySystem.ActorInfoTransaction.NativeRevocationBlocksOrdinaryCommit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAvatarActorInfoNativeRevocationBlocksOrdinaryCommit::RunTest(const FString& Parameters)
{
	using namespace GGYGOAvatarActorInfoTransactionTests;
	FNativeObservation Observation;
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	FGGYGOAvatarBindingResult First;
	FGGYGOAvatarBindingPublicationReceipt FirstPublication;
	if (!Bootstrap(*this, Fixture, First, FirstPublication) || !Fixture.GrantProbe(*this)) { return false; }
	const FGGYGOAvatarBindingRequest Request = Fixture.Request(EGGYGOAvatarBindingKind::Init, First.CommittedContext, Fixture.AvatarB);
	Fixture.Probe->ArmNativeAvatarHookForTest(FGGYGOAvatarActorInfoNativeHook::CreateLambda(
		[&](const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec)
		{
			Observation.Observe(Fixture, ActorInfo, Spec, Fixture.AvatarB);
			Observation.bCallerBefore = Request.IsRequestContextCurrent();
			Observation.bInvalidated = Fixture.ASC->InvalidateAvatarBinding(
				First.CommittedContext, EGGYGOAvatarBindingReason::OperationInvalidated, Observation.InvalidationReason);
			Observation.bCallerAfter = Request.IsRequestContextCurrent();
		}));
	FGGYGOAvatarBindingPublicationReceipt FailedPublication = FirstPublication;
	const FGGYGOAvatarBindingResult Failed = Fixture.ASC->TryExecuteAvatarActorInfoTransaction(Request, FailedPublication);
	if (!Observation.CheckCallback(*this, Fixture)
		|| !TestTrue(TEXT("Real native callback revoked exact Context"), Observation.bInvalidated)
		|| !TestEqual(TEXT("Native callback revocation reason None"), Observation.InvalidationReason, EGGYGOAvatarBindingReason::None)
		|| !TestTrue(TEXT("Caller pure query remains true"), Observation.bCallerBefore && Observation.bCallerAfter)
		|| !CheckFailure(*this, Failed, EGGYGOAvatarBindingOutcome::Stale, EGGYGOAvatarBindingReason::OperationInvalidated, true)
		|| !TestTrue(TEXT("Failure retains original Before"), Failed.Before.HasSameContext(First.CommittedContext))
		|| !CheckEmptyPublication(*this, FailedPublication, FirstPublication)
		|| !CheckActual(*this, Fixture, Fixture.Owner, Fixture.AvatarB)
		|| !TestFalse(TEXT("Failed native stack released Busy"), Fixture.ASC->IsAvatarBindingNativeWriteBusy())) { return false; }
	EGGYGOAvatarBindingReason Reason;
	if (!TestTrue(TEXT("No false successor Context committed"), Fixture.ASC->GetAvatarBindingContext().HasSameContext(First.CommittedContext))
		|| !TestEqual(TEXT("Explicit revocation stays authoritative"),
			Fixture.ASC->CheckAvatarBindingContext(First.CommittedContext, Reason), EGGYGOAvatarBindingOutcome::Stale)
		|| !TestEqual(TEXT("Context is revoked"), Reason, EGGYGOAvatarBindingReason::OperationInvalidated)) { return false; }

	const FGGYGOAvatarBindingRequest ReplacementRequest = Fixture.Request(EGGYGOAvatarBindingKind::Init, First.CommittedContext, Fixture.AvatarC);
	FGGYGOAvatarBindingPublicationReceipt ReplacementPublication;
	const FGGYGOAvatarBindingResult Replacement = Fixture.ASC->TryReplaceRevokedAvatarActorInfoTransaction(ReplacementRequest, ReplacementPublication);
	bool bOK = CheckCommit(*this, Fixture, Replacement, ReplacementPublication, First.CommittedContext,
		EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, Fixture.AvatarC);
	bOK &= CheckActual(*this, Fixture, Fixture.Owner, Fixture.AvatarC);
	bOK &= TestTrue(TEXT("Failed operation is not reused"), Replacement.Operation.Serial == Failed.Operation.Serial + 1);
	bOK &= TestEqual(TEXT("One-shot hook never replays"), Fixture.Probe->GetArmedNativeCallsForTest(), 1);
	bOK &= CheckHistory(*this, FirstPublication, First, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, Fixture.AvatarA);
	return bOK;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAvatarActorInfoNativeBusyWindow,
	"GGYGO.AbilitySystem.ActorInfoTransaction.NativeBusyWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAvatarActorInfoNativeBusyWindow::RunTest(const FString& Parameters)
{
	using namespace GGYGOAvatarActorInfoTransactionTests;
	FNativeObservation Observation;
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	FGGYGOAvatarBindingResult First;
	FGGYGOAvatarBindingPublicationReceipt FirstPublication;
	if (!Bootstrap(*this, Fixture, First, FirstPublication) || !Fixture.GrantProbe(*this)) { return false; }
	Observation.NestedPublication = FirstPublication;
	const FGGYGOAvatarBindingRequest OuterRequest = Fixture.Request(EGGYGOAvatarBindingKind::Init, First.CommittedContext, Fixture.AvatarB);
	Fixture.Probe->ArmNativeAvatarHookForTest(FGGYGOAvatarActorInfoNativeHook::CreateLambda(
		[&](const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec)
		{
			Observation.Observe(Fixture, ActorInfo, Spec, Fixture.AvatarB);
			const FGGYGOAvatarBindingRequest NestedRequest = Fixture.Request(EGGYGOAvatarBindingKind::Init, First.CommittedContext, Fixture.AvatarC);
			Observation.bCallerBefore = NestedRequest.IsRequestContextCurrent();
			Observation.NestedResult = Fixture.ASC->TryExecuteAvatarActorInfoTransaction(NestedRequest, Observation.NestedPublication);
			Observation.bBusyAfterNested = Fixture.ASC->IsAvatarBindingNativeWriteBusy();
		}));
	FGGYGOAvatarBindingPublicationReceipt OuterPublication;
	const FGGYGOAvatarBindingResult Outer = Fixture.ASC->TryExecuteAvatarActorInfoTransaction(OuterRequest, OuterPublication);
	if (!Observation.CheckCallback(*this, Fixture)
		|| !TestTrue(TEXT("Nested caller is still valid"), Observation.bCallerBefore)
		|| !TestTrue(TEXT("Rejected nested request does not close outer window"), Observation.bBusyAfterNested)
		|| !CheckFailure(*this, Observation.NestedResult, EGGYGOAvatarBindingOutcome::Busy, EGGYGOAvatarBindingReason::NativeWriteBusy, false)
		|| !CheckEmptyPublication(*this, Observation.NestedPublication, FirstPublication)
		|| !CheckCommit(*this, Fixture, Outer, OuterPublication, First.CommittedContext,
			EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, Fixture.AvatarB)
		|| !CheckActual(*this, Fixture, Fixture.Owner, Fixture.AvatarB)) { return false; }

	const FGGYGOAvatarBindingRequest NextRequest = Fixture.Request(EGGYGOAvatarBindingKind::Init, Outer.CommittedContext, Fixture.AvatarC);
	FGGYGOAvatarBindingPublicationReceipt NextPublication;
	const FGGYGOAvatarBindingResult Next = Fixture.ASC->TryExecuteAvatarActorInfoTransaction(NextRequest, NextPublication);
	bool bOK = CheckCommit(*this, Fixture, Next, NextPublication, Outer.CommittedContext,
		EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, Fixture.AvatarC);
	bOK &= CheckActual(*this, Fixture, Fixture.Owner, Fixture.AvatarC);
	bOK &= TestTrue(TEXT("Busy request consumed no serial and created no queued operation"), Next.Operation.Serial == Outer.Operation.Serial + 1);
	bOK &= TestEqual(TEXT("Hook stays disarmed after native return"), Fixture.Probe->GetArmedNativeCallsForTest(), 1);
	bOK &= CheckHistory(*this, OuterPublication, Outer, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, Fixture.AvatarB);
	return bOK;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAvatarBindingPublicationExactOnceAndSuccessor,
	"GGYGO.AbilitySystem.ActorInfoTransaction.PublicationExactOnceAndSuccessor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAvatarBindingPublicationExactOnceAndSuccessor::RunTest(const FString& Parameters)
{
	using namespace GGYGOAvatarActorInfoTransactionTests;
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	APawn* PawnA = Fixture.World->SpawnActor<APawn>();
	APawn* PawnB = Fixture.World->SpawnActor<APawn>();
	if (!TestNotNull(TEXT("Publication Pawn A"), PawnA)
		|| !TestNotNull(TEXT("Publication Pawn B"), PawnB)) { return false; }

	FGGYGOAvatarBindingResult CommitA;
	FGGYGOAvatarBindingResult CommitB;
	FGGYGOAvatarBindingResult RecursiveA;
	FGGYGOAvatarBindingPublicationReceipt ReceiptA;
	FGGYGOAvatarBindingPublicationReceipt ReceiptB;
	TWeakObjectPtr<UGGYGOAvatarBindingPublicationTestAbility> Probe;
	int32 NoticesA = 0;
	int32 NoticesB = 0;
	bool bCallbackOK = true;
	const auto QueryFor = [&Fixture](APawn* Pawn)
	{
		const bool* ScopeOpen = &Fixture.bCallerScopeOpen;
		const TWeakObjectPtr<UGGYGOAbilitySystemComponent> WeakASC(Fixture.ASC);
		const TWeakObjectPtr<AActor> WeakOwner(Fixture.Owner);
		const TWeakObjectPtr<APawn> WeakPawn(Pawn);
		return TFunction<bool()>([ScopeOpen, WeakASC, WeakOwner, WeakPawn]()
		{
			const AActor* Owner = WeakOwner.Get();
			const APawn* OriginalPawn = WeakPawn.Get();
			return *ScopeOpen && WeakASC.IsValid() && Owner && !Owner->IsActorBeingDestroyed()
				&& OriginalPawn && !OriginalPawn->IsActorBeingDestroyed();
		});
	};
	const TFunction<bool()> CallerA = QueryFor(PawnA);
	const TFunction<bool()> CallerB = QueryFor(PawnB);
	const auto CheckPublicationResult = [&](const FGGYGOAvatarBindingResult& Result,
		const FGGYGOAvatarBindingResult& Commit, EGGYGOAvatarBindingOutcome Outcome,
		EGGYGOAvatarBindingReason Reason)
	{
		bool bOK = TestEqual(TEXT("Exact publication outcome"), Result.Outcome, Outcome);
		bOK &= TestEqual(TEXT("Exact publication reason"), Result.Reason, Reason);
		bOK &= TestTrue(TEXT("Publication keeps historical committed flag"), Result.bCommitted);
		bOK &= TestTrue(TEXT("Publication keeps original operation"), Result.Operation.HasSameIdentity(Commit.Operation));
		bOK &= TestTrue(TEXT("Publication keeps original Before"), SameContextValue(Result.Before, Commit.Before));
		bOK &= TestTrue(TEXT("Publication keeps original commit"), Result.CommittedContext.HasSameContext(Commit.CommittedContext));
		return bOK;
	};

	// Declared after every event capture. Early returns retire the callback before captured data.
	struct FPublicationTestResources
	{
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
		FGameplayAbilitySpecHandle GrantedHandle;
		FDelegateHandle NoticeHandle;
		~FPublicationTestResources()
		{
			if (UGGYGOAbilitySystemComponent* LiveASC = ASC.Get())
			{
				if (NoticeHandle.IsValid()) { LiveASC->OnAvatarBindingNotice().Remove(NoticeHandle); }
				if (GrantedHandle.IsValid()) { LiveASC->ClearAbility(GrantedHandle); }
			}
		}
	} Resources;
	Resources.ASC = Fixture.ASC;
	Resources.GrantedHandle = Fixture.ASC->GiveAbility(FGameplayAbilitySpec(
		UGGYGOAvatarBindingPublicationTestAbility::StaticClass(), 1));
	{
		const FGameplayAbilitySpec* Spec = Fixture.ASC->FindAbilitySpecFromHandle(
			Resources.GrantedHandle, EConsiderPending::None);
		Probe = Spec ? Cast<UGGYGOAvatarBindingPublicationTestAbility>(Spec->GetPrimaryInstance()) : nullptr;
	}
	UGGYGOAvatarBindingPublicationTestAbility* Instance = Probe.Get();
	if (!TestTrue(TEXT("Publication probe is genuinely granted"), Resources.GrantedHandle.IsValid())
		|| !TestNotNull(TEXT("Publication probe has real primary instance"), Instance)
		|| !TestTrue(TEXT("Publication probe is not CDO"), Instance->IsInstantiated())
		|| !TestEqual(TEXT("Publication probe retains per-actor policy"), Instance->GetInstancingPolicy(),
			EGameplayAbilityInstancingPolicy::InstancedPerActor)
		|| !TestEqual(TEXT("Publication probe has no OnSpawn activation"), Instance->GetActivationPolicy(),
			EGGYGOAbilityActivationPolicy::OnInputTriggered)
		|| !TestEqual(TEXT("GiveAbility did not publish Pawn notice"), Instance->GetPawnNoticeCallsForTest(), 0)) { return false; }

	CommitA = Fixture.ASC->TryBootstrapAvatarActorInfoTransaction(
		Fixture.Request(EGGYGOAvatarBindingKind::Init, {}, PawnA), ReceiptA);
	if (!CheckCommit(*this, Fixture, CommitA, ReceiptA, {}, EGGYGOAvatarBindingNoticeKind::Initialized,
		Fixture.Owner, PawnA)
		|| !CheckActual(*this, Fixture, Fixture.Owner, PawnA)
		|| !TestFalse(TEXT("A Pending receipt is not Dispatching"), Fixture.ASC->IsAvatarBindingNoticeDispatching(ReceiptA))
		|| !TestFalse(TEXT("A Pending context is not published"), Fixture.ASC->IsAvatarBindingPublicationContextCurrent(CommitA.CommittedContext))
		|| !TestEqual(TEXT("Commit alone does not notify project GA"), Instance->GetPawnNoticeCallsForTest(), 0)) { return false; }

	Resources.NoticeHandle = Fixture.ASC->OnAvatarBindingNotice().AddLambda(
		[&](const FGGYGOAvatarBindingPublicationReceipt& Receipt, const FGGYGOAvatarBindingNotice& Notice)
		{
			if (Notice.After.HasSameContext(CommitA.CommittedContext))
			{
				++NoticesA;
				// Fail duplicate admission without recursing indefinitely if production regresses.
				if (NoticesA != 1)
				{
					bCallbackOK &= TestEqual(TEXT("A dispatch occurs exactly once"), NoticesA, 1);
					return;
				}
				bCallbackOK &= TestTrue(TEXT("A event authenticates exact receipt"), Fixture.ASC->IsAvatarBindingNoticeDispatching(Receipt));
				bCallbackOK &= TestTrue(TEXT("A original receipt authenticates during dispatch"), Fixture.ASC->IsAvatarBindingNoticeDispatching(ReceiptA));
				bCallbackOK &= TestTrue(TEXT("A context is published during event"), Fixture.ASC->IsAvatarBindingPublicationContextCurrent(CommitA.CommittedContext));
				bCallbackOK &= TestFalse(TEXT("Ordinary publication event has no native Busy"), Fixture.ASC->IsAvatarBindingNativeWriteBusy());
				bCallbackOK &= TestTrue(TEXT("Event Notice operation is A"), Notice.Operation.HasSameIdentity(CommitA.Operation));
				bCallbackOK &= CheckHistory(*this, Receipt, CommitA, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, PawnA);
				RecursiveA = Fixture.ASC->PublishAvatarBindingNotice(ReceiptA, CallerA);
				bCallbackOK &= CheckPublicationResult(RecursiveA, CommitA,
					EGGYGOAvatarBindingOutcome::Busy, EGGYGOAvatarBindingReason::PublicationInProgress);
				bCallbackOK &= TestEqual(TEXT("Recursive A did not dispatch"), NoticesA, 1);
				bCallbackOK &= TestTrue(TEXT("Recursive rejection preserves outer dispatch"), Fixture.ASC->IsAvatarBindingNoticeDispatching(ReceiptA));
				CommitB = Fixture.ASC->TryExecuteAvatarActorInfoTransaction(
					Fixture.Request(EGGYGOAvatarBindingKind::Init, CommitA.CommittedContext, PawnB), ReceiptB);
				bCallbackOK &= CheckCommit(*this, Fixture, CommitB, ReceiptB, CommitA.CommittedContext,
					EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, PawnB);
				bCallbackOK &= CheckActual(*this, Fixture, Fixture.Owner, PawnB);
				bCallbackOK &= TestTrue(TEXT("A caller stays valid after real B commit"), CallerA());
				bCallbackOK &= TestFalse(TEXT("B takeover invalidates A dispatch"), Fixture.ASC->IsAvatarBindingNoticeDispatching(ReceiptA));
				bCallbackOK &= TestFalse(TEXT("B remains Pending inside A event"), Fixture.ASC->IsAvatarBindingPublicationContextCurrent(CommitB.CommittedContext));
			}
			else if (Notice.After.HasSameContext(CommitB.CommittedContext))
			{
				++NoticesB;
				bCallbackOK &= TestEqual(TEXT("B dispatch occurs exactly once"), NoticesB, 1);
				bCallbackOK &= TestTrue(TEXT("B event authenticates exact receipt"), Fixture.ASC->IsAvatarBindingNoticeDispatching(Receipt));
				bCallbackOK &= TestTrue(TEXT("B context qualifies during event"), Fixture.ASC->IsAvatarBindingPublicationContextCurrent(CommitB.CommittedContext));
				bCallbackOK &= TestFalse(TEXT("B ordinary event has no native Busy"), Fixture.ASC->IsAvatarBindingNativeWriteBusy());
				bCallbackOK &= TestTrue(TEXT("Event Notice operation is B"), Notice.Operation.HasSameIdentity(CommitB.Operation));
				bCallbackOK &= CheckHistory(*this, Receipt, CommitB, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, PawnB);
			}
			else
			{
				bCallbackOK &= TestTrue(TEXT("Event must belong to original A or real B"), false);
			}
		});
	if (!TestTrue(TEXT("Real publication subscription installed"), Resources.NoticeHandle.IsValid())) { return false; }

	const FGGYGOAvatarBindingResult PublishedA = Fixture.ASC->PublishAvatarBindingNotice(ReceiptA, CallerA);
	Instance = Probe.Get();
	if (!bCallbackOK || !TestNotNull(TEXT("Granted probe survives A unwind"), Instance)
		|| !CheckPublicationResult(PublishedA, CommitA, EGGYGOAvatarBindingOutcome::Stale,
			EGGYGOAvatarBindingReason::ExpectedContextMismatch)
		|| !TestEqual(TEXT("A emitted exactly one ordinary event"), NoticesA, 1)
		|| !TestEqual(TEXT("B was not published inside A event"), NoticesB, 0)
		|| !TestEqual(TEXT("Stale A stopped before GA notification"), Instance->GetPawnNoticeCallsForTest(), 0)
		|| !TestTrue(TEXT("Pure A caller remains valid after unwind"), CallerA())
		|| !CheckActual(*this, Fixture, Fixture.Owner, PawnB)
		|| !TestTrue(TEXT("A unwind preserved actual B context"), Fixture.ASC->GetAvatarBindingContext().HasSameContext(CommitB.CommittedContext))
		|| !TestFalse(TEXT("Unpublished B is still not Dispatching"), Fixture.ASC->IsAvatarBindingNoticeDispatching(ReceiptB))
		|| !TestFalse(TEXT("A unwind did not convert B Pending to published"), Fixture.ASC->IsAvatarBindingPublicationContextCurrent(CommitB.CommittedContext))) { return false; }

	// A scope has fully returned. Only an intact B Pending resource can now publish successfully.
	const FGGYGOAvatarBindingResult PublishedB = Fixture.ASC->PublishAvatarBindingNotice(ReceiptB, CallerB);
	Instance = Probe.Get();
	if (!bCallbackOK || !TestNotNull(TEXT("Granted probe survives B publication"), Instance)
		|| !CheckPublicationResult(PublishedB, CommitB, EGGYGOAvatarBindingOutcome::Succeeded, EGGYGOAvatarBindingReason::None)
		|| !TestEqual(TEXT("B emitted exactly one ordinary event"), NoticesB, 1)
		|| !TestEqual(TEXT("Only B notified project GA once"), Instance->GetPawnNoticeCallsForTest(), 1)
		|| !TestTrue(TEXT("GA notification records exact Pawn B"),
			Instance->GetLastNotifiedAvatarForTest().HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(PawnB)))) { return false; }

	const FGGYGOAvatarBindingResult DuplicateB = Fixture.ASC->PublishAvatarBindingNotice(ReceiptB, CallerB);
	const FGGYGOAvatarBindingResult OldA = Fixture.ASC->PublishAvatarBindingNotice(ReceiptA, CallerA);
	bool bOK = bCallbackOK;
	bOK &= CheckPublicationResult(DuplicateB, CommitB, EGGYGOAvatarBindingOutcome::Rejected,
		EGGYGOAvatarBindingReason::PublicationAlreadyConsumed);
	bOK &= CheckPublicationResult(OldA, CommitA, EGGYGOAvatarBindingOutcome::Stale,
		EGGYGOAvatarBindingReason::ExpectedContextMismatch);
	bOK &= TestEqual(TEXT("A cannot replay event"), NoticesA, 1);
	bOK &= TestEqual(TEXT("Consumed B cannot replay event"), NoticesB, 1);
	bOK &= TestEqual(TEXT("Rejected replay cannot repeat GA notification"), Instance->GetPawnNoticeCallsForTest(), 1);
	bOK &= TestFalse(TEXT("Consumed B receipt is no longer Dispatching"), Fixture.ASC->IsAvatarBindingNoticeDispatching(ReceiptB));
	bOK &= TestTrue(TEXT("Replay failures preserve B Consumed qualification"), Fixture.ASC->IsAvatarBindingPublicationContextCurrent(CommitB.CommittedContext));
	bOK &= TestFalse(TEXT("Old A has no publication qualification"), Fixture.ASC->IsAvatarBindingPublicationContextCurrent(CommitA.CommittedContext));
	bOK &= CheckActual(*this, Fixture, Fixture.Owner, PawnB);
	bOK &= CheckHistory(*this, ReceiptA, CommitA, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, PawnA);
	bOK &= CheckHistory(*this, ReceiptB, CommitB, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, PawnB);
	return bOK;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAvatarBindingPublicationPendingRemoveOwnership,
	"GGYGO.AbilitySystem.ActorInfoTransaction.PublicationPendingRemoveOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAvatarBindingPublicationPendingRemoveOwnership::RunTest(const FString& Parameters)
{
	using namespace GGYGOAvatarActorInfoTransactionTests;
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	APawn* PawnA = Fixture.World->SpawnActor<APawn>();
	APawn* PawnB = Fixture.World->SpawnActor<APawn>();
	if (!TestNotNull(TEXT("Ownership Pawn A"), PawnA)
		|| !TestNotNull(TEXT("Ownership Pawn B"), PawnB)) { return false; }
	const FGGYGOAvatarBindingRequest InitA = Fixture.Request(EGGYGOAvatarBindingKind::Init, {}, PawnA);
	FGGYGOAvatarBindingResult CommitA;
	FGGYGOAvatarBindingPublicationReceipt ReceiptA;
	FGameplayAbilitySpecHandle FirstHandle;
	FGameplayAbilitySpecHandle TargetHandle;
	FGameplayAbilitySpecHandle SuccessorHandle;
	TWeakObjectPtr<UGGYGOAvatarBindingPublicationTestAbility> First;
	TWeakObjectPtr<UGGYGOAvatarBindingPublicationTestAbility> Target;
	TWeakObjectPtr<UGGYGOAvatarBindingPublicationTestAbility> Successor;
	int32 HookCalls = 0;
	bool bHookOK = true;
	const auto FindProbe = [&Fixture](FGameplayAbilitySpecHandle Handle)
	{
		const FGameplayAbilitySpec* Spec = Fixture.ASC->FindAbilitySpecFromHandle(Handle, EConsiderPending::None);
		return Spec ? Cast<UGGYGOAvatarBindingPublicationTestAbility>(Spec->GetPrimaryInstance()) : nullptr;
	};

	// Captures and Handle storage outlive this guard. The inner native list lock exits first.
	struct FPendingRemoveTestResources
	{
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
		TWeakObjectPtr<UGGYGOAvatarBindingPublicationTestAbility>& FirstProbe;
		const FGameplayAbilitySpecHandle& FirstGrant;
		const FGameplayAbilitySpecHandle& TargetGrant;
		const FGameplayAbilitySpecHandle& SuccessorGrant;
		~FPendingRemoveTestResources()
		{
			if (UGGYGOAvatarBindingPublicationTestAbility* Probe = FirstProbe.Get())
			{
				Probe->DisarmPawnNoticeHookForTest();
			}
			if (UGGYGOAbilitySystemComponent* LiveASC = ASC.Get())
			{
				for (FGameplayAbilitySpecHandle Handle : {FirstGrant, TargetGrant, SuccessorGrant})
				{
					if (Handle.IsValid()) { LiveASC->ClearAbility(Handle); }
				}
			}
		}
	} Resources{TWeakObjectPtr<UGGYGOAbilitySystemComponent>(Fixture.ASC), First, FirstHandle, TargetHandle, SuccessorHandle};
	FirstHandle = Fixture.ASC->GiveAbility(FGameplayAbilitySpec(
		UGGYGOAvatarBindingPublicationTestAbility::StaticClass(), 1));
	TargetHandle = Fixture.ASC->GiveAbility(FGameplayAbilitySpec(
		UGGYGOAvatarBindingPublicationTestAbility::StaticClass(), 1));
	First = FindProbe(FirstHandle);
	Target = FindProbe(TargetHandle);
	if (!TestTrue(TEXT("Two genuine grant handles"), FirstHandle.IsValid() && TargetHandle.IsValid())
		|| !TestTrue(TEXT("Distinct original grant handles"), FirstHandle != TargetHandle)
		|| !TestNotNull(TEXT("First real primary instance"), First.Get())
		|| !TestNotNull(TEXT("Target real primary instance"), Target.Get())
		|| !TestTrue(TEXT("Independent real instances"), First.Get() != Target.Get())
		|| !TestTrue(TEXT("Both probes are instantiated"), First->IsInstantiated() && Target->IsInstantiated())
		|| !TestEqual(TEXT("First keeps per-actor policy"), First->GetInstancingPolicy(), EGameplayAbilityInstancingPolicy::InstancedPerActor)
		|| !TestEqual(TEXT("Target keeps per-actor policy"), Target->GetInstancingPolicy(), EGameplayAbilityInstancingPolicy::InstancedPerActor)
		|| !TestEqual(TEXT("First is not an OnSpawn ability"), First->GetActivationPolicy(), EGGYGOAbilityActivationPolicy::OnInputTriggered)
		|| !TestEqual(TEXT("Target is not an OnSpawn ability"), Target->GetActivationPolicy(), EGGYGOAbilityActivationPolicy::OnInputTriggered)) { return false; }
	{
		const UAbilitySystemComponent* ReadASC = Fixture.ASC;
		const TArray<FGameplayAbilitySpec>& Granted = ReadASC->GetActivatableAbilities();
		if (!TestEqual(TEXT("Only two current grants before capture"), Granted.Num(), 2)
			|| !TestTrue(TEXT("First precedes Target in actual native list"),
				Granted[0].Handle == FirstHandle && Granted[1].Handle == TargetHandle)) { return false; }
	}
	CommitA = Fixture.ASC->TryBootstrapAvatarActorInfoTransaction(InitA, ReceiptA);
	if (!CheckCommit(*this, Fixture, CommitA, ReceiptA, {}, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, PawnA)
		|| !CheckActual(*this, Fixture, Fixture.Owner, PawnA)
		|| !TestEqual(TEXT("First has no pre-publication notice"), First->GetPawnNoticeCallsForTest(), 0)
		|| !TestEqual(TEXT("Target has no pre-publication notice"), Target->GetPawnNoticeCallsForTest(), 0)) { return false; }

	First->ArmPawnNoticeHookForTest(FGGYGOAvatarBindingPublicationPawnHook::CreateLambda([&]()
	{
		++HookCalls;
		bHookOK &= TestEqual(TEXT("Ownership hook executes once"), HookCalls, 1);
		bHookOK &= TestTrue(TEXT("Hook runs during authentic A dispatch"), Fixture.ASC->IsAvatarBindingNoticeDispatching(ReceiptA));
		bHookOK &= TestFalse(TEXT("OnPawn publication callback is outside native Busy"), Fixture.ASC->IsAvatarBindingNativeWriteBusy());
		UGGYGOAvatarBindingPublicationTestAbility* LiveTarget = Target.Get();
		if (!TestNotNull(TEXT("Original Target is alive before ClearAbility"), LiveTarget))
		{
			bHookOK = false;
			return;
		}
		bHookOK &= TestEqual(TEXT("Target not notified before removal request"), LiveTarget->GetPawnNoticeCallsForTest(), 0);
		Fixture.ASC->ClearAbility(TargetHandle);
		LiveTarget = Target.Get();
		if (!TestNotNull(TEXT("PendingRemove Target must remain weak-valid"), LiveTarget))
		{
			bHookOK = false;
			return; // An immediately destroyed object cannot satisfy this ownership reproduction.
		}
		{
			const FGameplayAbilitySpec* Pending = Fixture.ASC->FindAbilitySpecFromHandle(
				TargetHandle, EConsiderPending::PendingRemove);
			bHookOK &= TestNotNull(TEXT("Original Target Spec remains under native list lock"), Pending);
			bHookOK &= TestTrue(TEXT("Native ClearAbility marked the original Spec PendingRemove"),
				Pending && Pending->PendingRemove && Pending->GetPrimaryInstance() == LiveTarget);
		}
		bHookOK &= TestNull(TEXT("Original Target is no longer an eligible grant"),
			Fixture.ASC->FindAbilitySpecFromHandle(TargetHandle, EConsiderPending::None));
		bHookOK &= TestTrue(TEXT("Removal does not close the pure caller"), InitA.IsRequestContextCurrent());
		EGGYGOAvatarBindingReason ContextReason;
		bHookOK &= TestEqual(TEXT("Removal keeps the original current binding"),
			Fixture.ASC->CheckAvatarBindingContext(CommitA.CommittedContext, ContextReason), EGGYGOAvatarBindingOutcome::Succeeded);
		bHookOK &= TestEqual(TEXT("Original context remains valid for the real reason"), ContextReason, EGGYGOAvatarBindingReason::None);
		SuccessorHandle = Fixture.ASC->GiveAbility(FGameplayAbilitySpec(
			UGGYGOAvatarBindingPublicationTestAbility::StaticClass(), 1));
		bHookOK &= TestTrue(TEXT("Native delayed grant has a distinct original handle"),
			SuccessorHandle.IsValid() && SuccessorHandle != TargetHandle && SuccessorHandle != FirstHandle);
		bHookOK &= TestNull(TEXT("Native pending add is not a current candidate"),
			Fixture.ASC->FindAbilitySpecFromHandle(SuccessorHandle, EConsiderPending::None));
	}));

	{
		FScopedAbilityListLock NativeListLock(*Fixture.ASC);
		const FGGYGOAvatarBindingResult PublishedA = Fixture.ASC->PublishAvatarBindingNotice(
			ReceiptA, InitA.IsRequestContextCurrent);
		if (!bHookOK || !TestEqual(TEXT("Real First OnPawn invoked the hook"), HookCalls, 1)
			|| !CheckCommit(*this, Fixture, PublishedA, ReceiptA, {}, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, PawnA)
			|| !TestNotNull(TEXT("First remains granted inside lock"), First.Get())
			|| !TestNotNull(TEXT("Target is still weak-valid after candidate iteration"), Target.Get())
			|| !TestEqual(TEXT("Eligible First notified once"), First->GetPawnNoticeCallsForTest(), 1)
			|| !TestEqual(TEXT("Live PendingRemove Target receives zero notices"), Target->GetPawnNoticeCallsForTest(), 0)
			|| !TestNull(TEXT("Target remains ineligible at publication return"),
				Fixture.ASC->FindAbilitySpecFromHandle(TargetHandle, EConsiderPending::None))
			|| !TestTrue(TEXT("Resource removal still allows authentic A consumption"),
				Fixture.ASC->IsAvatarBindingPublicationContextCurrent(CommitA.CommittedContext))) { return false; }
	} // Native unlock applies the successor add, then clears only the original Target Handle.

	Successor = FindProbe(SuccessorHandle);
	if (!TestNull(TEXT("Original Target Handle is fully removed after unlock"),
		Fixture.ASC->FindAbilitySpecFromHandle(TargetHandle, EConsiderPending::PendingRemove))
		|| !TestNotNull(TEXT("First remains weak-valid after native unlock"), First.Get())
		|| !TestTrue(TEXT("First grant is preserved by old removal"), FindProbe(FirstHandle) == First.Get())
		|| !TestNotNull(TEXT("Successor is now a genuine primary instance"), Successor.Get())
		|| !TestTrue(TEXT("Successor is an instantiated resource"), Successor->IsInstantiated())
		|| !TestTrue(TEXT("Successor is distinct from retained First"), Successor.Get() != First.Get())
		|| !TestEqual(TEXT("Pending successor received no old A notification"), Successor->GetPawnNoticeCallsForTest(), 0)
		|| !CheckActual(*this, Fixture, Fixture.Owner, PawnA)) { return false; }

	const FGGYGOAvatarBindingRequest InitB = Fixture.Request(EGGYGOAvatarBindingKind::Init, CommitA.CommittedContext, PawnB);
	FGGYGOAvatarBindingPublicationReceipt ReceiptB;
	const FGGYGOAvatarBindingResult CommitB = Fixture.ASC->TryExecuteAvatarActorInfoTransaction(InitB, ReceiptB);
	if (!CheckCommit(*this, Fixture, CommitB, ReceiptB, CommitA.CommittedContext,
		EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, PawnB)) { return false; }
	const FGGYGOAvatarBindingResult PublishedB = Fixture.ASC->PublishAvatarBindingNotice(
		ReceiptB, InitB.IsRequestContextCurrent);
	if (!CheckCommit(*this, Fixture, PublishedB, ReceiptB, CommitA.CommittedContext,
		EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, PawnB)
		|| !TestNotNull(TEXT("First remains alive after B publication"), First.Get())
		|| !TestNotNull(TEXT("Successor remains alive after B publication"), Successor.Get())) { return false; }
	bool bOK = TestEqual(TEXT("First notified for A and B"), First->GetPawnNoticeCallsForTest(), 2);
	bOK &= TestEqual(TEXT("Successor notified only for legitimate B"), Successor->GetPawnNoticeCallsForTest(), 1);
	bOK &= TestEqual(TEXT("The old one-shot hook never replays"), HookCalls, 1);
	bOK &= TestTrue(TEXT("First records exact B Avatar"),
		First->GetLastNotifiedAvatarForTest().HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(PawnB)));
	bOK &= TestTrue(TEXT("Successor records exact B Avatar"),
		Successor->GetLastNotifiedAvatarForTest().HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(PawnB)));
	bOK &= TestTrue(TEXT("Old removal preserves First membership"), FindProbe(FirstHandle) == First.Get());
	bOK &= TestTrue(TEXT("Old removal preserves exact successor membership"), FindProbe(SuccessorHandle) == Successor.Get());
	bOK &= TestNull(TEXT("Old Target Handle cannot reappear"), Fixture.ASC->FindAbilitySpecFromHandle(TargetHandle));
	bOK &= TestTrue(TEXT("Legitimate B publication remains current"), Fixture.ASC->IsAvatarBindingPublicationContextCurrent(CommitB.CommittedContext));
	bOK &= CheckActual(*this, Fixture, Fixture.Owner, PawnB);
	bOK &= CheckHistory(*this, ReceiptA, CommitA, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, PawnA);
	bOK &= CheckHistory(*this, ReceiptB, CommitB, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, PawnB);
	return bOK;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAvatarBindingCancelNativeFilteringAndBusy,
	"GGYGO.AbilitySystem.ActorInfoTransaction.CancelNativeFilteringAndBusy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAvatarBindingCancelNativeFilteringAndBusy::RunTest(const FString& Parameters)
{
	using namespace GGYGOAvatarActorInfoTransactionTests;
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	const FGGYGOAvatarBindingRequest InitA = Fixture.Request(
		EGGYGOAvatarBindingKind::Init, {}, Fixture.AvatarA);
	FGGYGOAvatarBindingResult CommitA;
	FGGYGOAvatarBindingResult CommitB;
	FGGYGOAvatarBindingResult NestedBind;
	FGGYGOAvatarBindingPublicationReceipt ReceiptA;
	FGGYGOAvatarBindingPublicationReceipt ReceiptB;
	FGGYGOAvatarBindingPublicationReceipt NestedReceipt;
	FGameplayAbilitySpecHandle ProbeHandle;
	TWeakObjectPtr<UGGYGOAvatarBindingCancelTestAbility> Probe;
	int32 CancelledCalls = 0;
	int32 EndedCalls = 0;
	int32 NoticesA = 0;
	int32 NoticesB = 0;
	bool bCallbackOK = true;
	const auto CheckProbeActive = [&](bool bExpectedActive)
	{
		const FGameplayAbilitySpec* Spec = Fixture.ASC->FindAbilitySpecFromHandle(
			ProbeHandle, EConsiderPending::None);
		UGGYGOAvatarBindingCancelTestAbility* Instance = Probe.Get();
		if (!TestNotNull(TEXT("Original native cancellation grant remains present"), Spec)
			|| !TestNotNull(TEXT("Original native cancellation instance remains alive"), Instance)) { return false; }
		bool bOK = TestTrue(TEXT("Grant retains exact original primary instance"), Spec->GetPrimaryInstance() == Instance);
		bOK &= TestTrue(TEXT("Grant retains exact original Handle"), Spec->Handle == ProbeHandle);
		bOK &= TestEqual(TEXT("Actual native Spec activity"), Spec->IsActive(), bExpectedActive);
		bOK &= TestEqual(TEXT("Actual native instance activity"), Instance->IsActive(), bExpectedActive);
		return bOK;
	};
	const auto CheckPublicationResult = [&](const FGGYGOAvatarBindingResult& Result,
		const FGGYGOAvatarBindingResult& Commit, EGGYGOAvatarBindingOutcome Outcome,
		EGGYGOAvatarBindingReason Reason)
	{
		bool bOK = TestEqual(TEXT("Exact publication outcome"), Result.Outcome, Outcome);
		bOK &= TestEqual(TEXT("Exact publication reason"), Result.Reason, Reason);
		bOK &= TestTrue(TEXT("Publication keeps historical commit"), Result.bCommitted);
		bOK &= TestTrue(TEXT("Publication keeps original operation"), Result.Operation.HasSameIdentity(Commit.Operation));
		bOK &= TestTrue(TEXT("Publication keeps original Before"), SameContextValue(Result.Before, Commit.Before));
		bOK &= TestTrue(TEXT("Publication keeps original commit"), Result.CommittedContext.HasSameContext(Commit.CommittedContext));
		return bOK;
	};
	const auto CheckCancelResult = [&](const FGGYGOAvatarBindingResult& Result)
	{
		bool bOK = TestEqual(TEXT("Cancellation synchronously succeeded"), Result.Outcome, EGGYGOAvatarBindingOutcome::Succeeded);
		bOK &= TestEqual(TEXT("Cancellation reason None"), Result.Reason, EGGYGOAvatarBindingReason::None);
		bOK &= TestTrue(TEXT("Cancellation owns an ASC-issued operation"),
			Result.Operation.HasIssuedIdentity() && Result.Operation.Issuer.Get() == Fixture.ASC);
		bOK &= TestTrue(TEXT("Cancellation operation follows original write"),
			Result.Operation.Serial > CommitA.CommittedContext.LastActorInfoWrite.Serial);
		bOK &= TestTrue(TEXT("Cancellation retains exact original Before"), Result.Before.HasSameContext(CommitA.CommittedContext));
		bOK &= TestFalse(TEXT("Cancellation never commits binding"), Result.bCommitted);
		bOK &= TestTrue(TEXT("Cancellation has no committed context"), IsEmptyContext(Result.CommittedContext));
		bOK &= TestTrue(TEXT("Cancellation preserves exact Binding and LastWrite"),
			Fixture.ASC->GetAvatarBindingContext().HasSameContext(CommitA.CommittedContext));
		EGGYGOAvatarBindingReason ContextReason;
		bOK &= TestEqual(TEXT("Original context still validates after cancellation"),
			Fixture.ASC->CheckAvatarBindingContext(CommitA.CommittedContext, ContextReason), EGGYGOAvatarBindingOutcome::Succeeded);
		bOK &= TestEqual(TEXT("Original context validation reason None"), ContextReason, EGGYGOAvatarBindingReason::None);
		bOK &= CheckActual(*this, Fixture, Fixture.Owner, Fixture.AvatarA);
		bOK &= TestFalse(TEXT("Native Busy releases only after complete return"), Fixture.ASC->IsAvatarBindingNativeWriteBusy());
		bOK &= TestFalse(TEXT("Matching publication permission is closed"),
			Fixture.ASC->IsAvatarBindingPublicationContextCurrent(CommitA.CommittedContext));
		bOK &= TestEqual(TEXT("Cancellation does not emit A notice"), NoticesA, 1);
		bOK &= TestEqual(TEXT("Cancellation does not emit B notice"), NoticesB, 0);
		return bOK;
	};

	// Every capture and Handle precedes this guard; delegates retire before grant/world cleanup.
	struct FCancelTestResources
	{
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
		TWeakObjectPtr<UGGYGOAvatarBindingCancelTestAbility>& OriginalProbe;
		const FGameplayAbilitySpecHandle& OriginalHandle;
		FDelegateHandle CancelledHandle;
		FDelegateHandle EndedHandle;
		FDelegateHandle NoticeHandle;
		~FCancelTestResources()
		{
			if (UGGYGOAvatarBindingCancelTestAbility* Instance = OriginalProbe.Get())
			{
				if (CancelledHandle.IsValid()) { Instance->OnGameplayAbilityCancelled.Remove(CancelledHandle); }
				if (EndedHandle.IsValid()) { Instance->OnGameplayAbilityEndedWithData.Remove(EndedHandle); }
			}
			if (UGGYGOAbilitySystemComponent* LiveASC = ASC.Get())
			{
				if (NoticeHandle.IsValid()) { LiveASC->OnAvatarBindingNotice().Remove(NoticeHandle); }
				if (OriginalHandle.IsValid()) { LiveASC->ClearAbility(OriginalHandle); }
			}
		}
	} Resources{TWeakObjectPtr<UGGYGOAbilitySystemComponent>(Fixture.ASC), Probe, ProbeHandle};
	ProbeHandle = Fixture.ASC->GiveAbility(FGameplayAbilitySpec(
		UGGYGOAvatarBindingCancelTestAbility::StaticClass(), 1));
	{
		const FGameplayAbilitySpec* Spec = Fixture.ASC->FindAbilitySpecFromHandle(
			ProbeHandle, EConsiderPending::None);
		Probe = Spec ? Cast<UGGYGOAvatarBindingCancelTestAbility>(Spec->GetPrimaryInstance()) : nullptr;
	}
	if (!TestTrue(TEXT("Cancellation uses genuine GiveAbility Handle"), ProbeHandle.IsValid())
		|| !TestNotNull(TEXT("Cancellation uses real primary instance"), Probe.Get())
		|| !TestTrue(TEXT("Cancellation instance is not the CDO"), Probe->IsInstantiated())
		|| !TestEqual(TEXT("Cancellation uses native per-actor policy"), Probe->GetInstancingPolicy(),
			EGameplayAbilityInstancingPolicy::InstancedPerActor)
		|| !TestEqual(TEXT("Cancellation probe activates on authority"), Probe->GetNetExecutionPolicy(),
			EGameplayAbilityNetExecutionPolicy::ServerOnly)
		|| !CheckProbeActive(false)) { return false; }
	Resources.NoticeHandle = Fixture.ASC->OnAvatarBindingNotice().AddLambda(
		[&](const FGGYGOAvatarBindingPublicationReceipt&, const FGGYGOAvatarBindingNotice& Notice)
		{
			if (Notice.After.HasSameContext(CommitA.CommittedContext))
			{
				++NoticesA;
				bCallbackOK &= TestEqual(TEXT("A has exactly one real publication"), NoticesA, 1);
			}
			else if (Notice.After.HasSameContext(CommitB.CommittedContext))
			{
				++NoticesB;
				bCallbackOK &= TestEqual(TEXT("B has exactly one real publication"), NoticesB, 1);
			}
			else { bCallbackOK &= TestTrue(TEXT("Notice must be original A or legal B"), false); }
		});
	if (!TestTrue(TEXT("Real notice subscription installed"), Resources.NoticeHandle.IsValid())) { return false; }
	CommitA = Fixture.ASC->TryBootstrapAvatarActorInfoTransaction(InitA, ReceiptA);
	if (!CheckCommit(*this, Fixture, CommitA, ReceiptA, {}, EGGYGOAvatarBindingNoticeKind::Initialized,
		Fixture.Owner, Fixture.AvatarA)) { return false; }
	const FGGYGOAvatarBindingResult PublishedA = Fixture.ASC->PublishAvatarBindingNotice(
		ReceiptA, InitA.IsRequestContextCurrent);
	if (!bCallbackOK || !CheckPublicationResult(PublishedA, CommitA,
		EGGYGOAvatarBindingOutcome::Succeeded, EGGYGOAvatarBindingReason::None)
		|| !TestTrue(TEXT("Original A publication qualifies before cancel"),
			Fixture.ASC->IsAvatarBindingPublicationContextCurrent(CommitA.CommittedContext))
		|| !TestTrue(TEXT("Original avatar is authority"), Fixture.AvatarA->HasAuthority())
		|| !TestTrue(TEXT("Real GAS activation succeeds"), Fixture.ASC->TryActivateAbility(ProbeHandle))
		|| !CheckProbeActive(true)) { return false; }
	Resources.CancelledHandle = Probe->OnGameplayAbilityCancelled.AddLambda([&]()
	{
		++CancelledCalls;
		bCallbackOK &= TestEqual(TEXT("Real native Cancelled callback occurs once"), CancelledCalls, 1);
		bCallbackOK &= TestTrue(TEXT("Cancelled callback runs inside native Busy"), Fixture.ASC->IsAvatarBindingNativeWriteBusy());
		bCallbackOK &= CheckActual(*this, Fixture, Fixture.Owner, Fixture.AvatarA);
		NestedReceipt = ReceiptA; // Busy rejection must reset authentic prior proof.
		NestedBind = Fixture.ASC->TryExecuteAvatarActorInfoTransaction(
			Fixture.Request(EGGYGOAvatarBindingKind::Init, CommitA.CommittedContext, Fixture.AvatarB), NestedReceipt);
		bCallbackOK &= CheckFailure(*this, NestedBind,
			EGGYGOAvatarBindingOutcome::Busy, EGGYGOAvatarBindingReason::NativeWriteBusy, false);
		bCallbackOK &= CheckEmptyPublication(*this, NestedReceipt, ReceiptA);
		bCallbackOK &= TestTrue(TEXT("Nested binding rejection preserves outer Busy"), Fixture.ASC->IsAvatarBindingNativeWriteBusy());
		bCallbackOK &= TestTrue(TEXT("Nested binding cannot replace original context"),
			Fixture.ASC->GetAvatarBindingContext().HasSameContext(CommitA.CommittedContext));
		bCallbackOK &= CheckActual(*this, Fixture, Fixture.Owner, Fixture.AvatarA);
	});
	Resources.EndedHandle = Probe->OnGameplayAbilityEndedWithData.AddLambda([&](const FAbilityEndedData& Data)
	{
		++EndedCalls;
		bCallbackOK &= TestEqual(TEXT("Real native Ended callback occurs once"), EndedCalls, 1);
		bCallbackOK &= TestEqual(TEXT("Native Cancelled precedes Ended"), CancelledCalls, 1);
		bCallbackOK &= TestTrue(TEXT("Native End reports exact original instance"), Data.AbilityThatEnded.Get() == Probe.Get());
		bCallbackOK &= TestTrue(TEXT("Native End reports exact original Handle"), Data.AbilitySpecHandle == ProbeHandle);
		bCallbackOK &= TestTrue(TEXT("Native End reports cancellation"), Data.bWasCancelled);
		bCallbackOK &= TestTrue(TEXT("Ended callback remains inside native Busy"), Fixture.ASC->IsAvatarBindingNativeWriteBusy());
		bCallbackOK &= CheckActual(*this, Fixture, Fixture.Owner, Fixture.AvatarA);
	});
	if (!TestTrue(TEXT("Real Cancelled subscription installed"), Resources.CancelledHandle.IsValid())
		|| !TestTrue(TEXT("Real Ended subscription installed"), Resources.EndedHandle.IsValid())) { return false; }

	// Non-null empty include filter matches no active ability; null retains native match-all mode.
	const FGameplayTagContainer EmptyWith;
	const FGGYGOAvatarBindingResult EmptyResult = Fixture.ASC->TryCancelAvatarBindingAbilities(
		CommitA.CommittedContext, &EmptyWith, nullptr, InitA.IsRequestContextCurrent);
	if (!bCallbackOK || !CheckCancelResult(EmptyResult) || !CheckProbeActive(true)
		|| !TestEqual(TEXT("Empty WithTags cannot trigger native Cancelled"), CancelledCalls, 0)
		|| !TestEqual(TEXT("Empty WithTags cannot trigger native Ended"), EndedCalls, 0)) { return false; }
	const FGGYGOAvatarBindingResult NullResult = Fixture.ASC->TryCancelAvatarBindingAbilities(
		CommitA.CommittedContext, nullptr, nullptr, InitA.IsRequestContextCurrent);
	if (!bCallbackOK || !CheckCancelResult(NullResult) || !CheckProbeActive(false)
		|| !TestEqual(TEXT("Null WithTags reaches real native Cancelled"), CancelledCalls, 1)
		|| !TestEqual(TEXT("Null WithTags reaches real native Ended"), EndedCalls, 1)
		|| !TestFalse(TEXT("Two cancellations use distinct original operations"),
			NullResult.Operation.HasSameIdentity(EmptyResult.Operation))) { return false; }

	const FGGYGOAvatarBindingResult ReplayA = Fixture.ASC->PublishAvatarBindingNotice(
		ReceiptA, InitA.IsRequestContextCurrent);
	if (!bCallbackOK || !CheckPublicationResult(ReplayA, CommitA,
		EGGYGOAvatarBindingOutcome::Stale, EGGYGOAvatarBindingReason::OperationInvalidated)
		|| !TestEqual(TEXT("Closed A receipt cannot replay notice"), NoticesA, 1)
		|| !CheckHistory(*this, ReceiptA, CommitA, EGGYGOAvatarBindingNoticeKind::Initialized,
			Fixture.Owner, Fixture.AvatarA)) { return false; }
	CommitB = Fixture.ASC->TryExecuteAvatarActorInfoTransaction(
		Fixture.Request(EGGYGOAvatarBindingKind::Init, CommitA.CommittedContext, Fixture.AvatarB), ReceiptB);
	if (!CheckCommit(*this, Fixture, CommitB, ReceiptB, CommitA.CommittedContext,
		EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, Fixture.AvatarB)
		|| !CheckActual(*this, Fixture, Fixture.Owner, Fixture.AvatarB)) { return false; }
	const FGGYGOAvatarBindingResult PublishedB = Fixture.ASC->PublishAvatarBindingNotice(
		ReceiptB, InitA.IsRequestContextCurrent);
	bool bOK = bCallbackOK;
	bOK &= CheckPublicationResult(PublishedB, CommitB,
		EGGYGOAvatarBindingOutcome::Succeeded, EGGYGOAvatarBindingReason::None);
	bOK &= TestEqual(TEXT("A cancellation never generates an extra notice"), NoticesA, 1);
	bOK &= TestEqual(TEXT("Only legal successor publication generates B notice"), NoticesB, 1);
	bOK &= TestTrue(TEXT("Legal B retains current publication permission"),
		Fixture.ASC->IsAvatarBindingPublicationContextCurrent(CommitB.CommittedContext));
	bOK &= TestFalse(TEXT("Legal B returns with native Busy released"), Fixture.ASC->IsAvatarBindingNativeWriteBusy());
	bOK &= CheckActual(*this, Fixture, Fixture.Owner, Fixture.AvatarB);
	bOK &= CheckProbeActive(false);
	bOK &= CheckHistory(*this, ReceiptA, CommitA, EGGYGOAvatarBindingNoticeKind::Initialized,
		Fixture.Owner, Fixture.AvatarA);
	bOK &= CheckHistory(*this, ReceiptB, CommitB, EGGYGOAvatarBindingNoticeKind::Initialized,
		Fixture.Owner, Fixture.AvatarB);
	return bOK;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAvatarBindingCueNativeRemovedAndBusy,
	"GGYGO.AbilitySystem.ActorInfoTransaction.CueNativeRemovedAndBusy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAvatarBindingCueNativeRemovedAndBusy::RunTest(const FString& Parameters)
{
	using namespace GGYGOAvatarActorInfoTransactionTests;
	FFixture Fixture;
	if (!Fixture.Initialize(*this)) { return false; }
	const FGameplayTag CueTag = TAG_GGYGO_ActorInfoTransaction_CueRemovalProbe.GetTag();
	UGameplayCueManager* Manager = UAbilitySystemGlobals::Get().GetGameplayCueManager();
	UGameplayCueSet* CueSet = Manager ? Manager->GetRuntimeCueSet() : nullptr;
	UClass* NotifyClass = UGGYGOAvatarBindingCueRemovalTestNotify::StaticClass();
	UGGYGOAvatarBindingCueRemovalTestNotify* Notify = GetMutableDefault<UGGYGOAvatarBindingCueRemovalTestNotify>();
	const FSoftObjectPath NotifyPath(NotifyClass);
	if (!TestTrue(TEXT("Dedicated native Cue tag is registered"), CueTag.IsValid())
		|| !TestNotNull(TEXT("Real configured CueManager"), Manager)
		|| !TestNotNull(TEXT("Real runtime CueSet"), CueSet)
		|| !TestNotNull(TEXT("Loaded native Notify CDO"), Notify)
		|| !TestTrue(TEXT("Native Notify path resolves without an async asset"), NotifyPath.ResolveObject() == NotifyClass)
		|| !TestFalse(TEXT("Real manager must not suppress the original target"), Manager->ShouldSuppressGameplayCues(Fixture.AvatarA))
		|| !TestTrue(TEXT("Original Cue owner is authoritative"), Fixture.Owner->HasAuthority())
		|| !TestTrue(TEXT("Probe preserves native override policy"), Notify->IsOverride)
		|| !TestFalse(TEXT("Notify hook has no existing borrower"), Notify->IsRemovedHookArmedForTest())
		|| !TestTrue(TEXT("Dedicated Tag has no descendant configuration"),
			UGameplayTagsManager::Get().RequestGameplayTagChildren(CueTag).IsEmpty())
		|| !TestEqual(TEXT("Fixture starts without its dedicated Cue"), Fixture.ASC->GetTagCount(CueTag), 0)) { return false; }
	for (const FGameplayCueNotifyData& Data : CueSet->GameplayCueData)
	{
		if (!TestFalse(TEXT("Dedicated Tag must not replace an existing direct mapping"),
			Data.GameplayCueTag == CueTag)) { return false; }
	}

	struct FCueConfigurationSnapshot
	{
		TArray<FString> DataRows;
		TArray<FString> ValidRouteRows;
		bool bValid = true;
	};
	const auto SnapshotConfiguration = [&](const UGameplayCueSet& Set)
	{
		FCueConfigurationSnapshot Snapshot;
		const auto DataKey = [](const FGameplayCueNotifyData& Data)
		{
			return Data.GameplayCueTag.ToString() + TEXT("|") + Data.GameplayCueNotifyObj.ToString();
		};
		for (const FGameplayCueNotifyData& Data : Set.GameplayCueData)
		{
			if (Data.GameplayCueTag == CueTag) { continue; } // Only this test's direct registration.
			FString Parent = TEXT("<none>");
			if (Data.ParentDataIdx != INDEX_NONE)
			{
				if (!Set.GameplayCueData.IsValidIndex(Data.ParentDataIdx)) { Snapshot.bValid = false; }
				else { Parent = DataKey(Set.GameplayCueData[Data.ParentDataIdx]); }
			}
			const FString Loaded = FString::Printf(TEXT("%p"), static_cast<void*>(Data.LoadedGameplayCueClass.Get()));
			Snapshot.DataRows.Add(DataKey(Data) + TEXT("|") + Loaded + TEXT("|") + Parent);
		}
		for (const TPair<FGameplayTag, int32>& Route : Set.GameplayCueDataMap)
		{
			if (Route.Value == INDEX_NONE) { continue; } // No Notify is the same semantic result as an absent route.
			if (!Set.GameplayCueData.IsValidIndex(Route.Value)) { Snapshot.bValid = false; continue; }
			Snapshot.ValidRouteRows.Add(Route.Key.ToString() + TEXT(">") + DataKey(Set.GameplayCueData[Route.Value]));
		}
		Snapshot.DataRows.Sort();
		Snapshot.ValidRouteRows.Sort();
		return Snapshot;
	};
	const FCueConfigurationSnapshot BeforeConfiguration = SnapshotConfiguration(*CueSet);
	if (!TestTrue(TEXT("Original Cue data and route indices are valid"), BeforeConfiguration.bValid)) { return false; }
	const FGGYGOAvatarBindingRequest InitA = Fixture.Request(EGGYGOAvatarBindingKind::Init, {}, Fixture.AvatarA);
	FGGYGOAvatarBindingResult SeedCommit;
	FGGYGOAvatarBindingResult CommitA;
	FGGYGOAvatarBindingResult CommitB;
	FGGYGOAvatarBindingResult NestedBind;
	FGGYGOAvatarBindingPublicationReceipt SeedReceipt;
	FGGYGOAvatarBindingPublicationReceipt ReceiptA;
	FGGYGOAvatarBindingPublicationReceipt ReceiptB;
	FGGYGOAvatarBindingPublicationReceipt NestedReceipt;
	const int32 WhileActiveBefore = Notify->GetWhileActiveCallsForTest();
	const int32 RemovedBefore = Notify->GetRemovedCallsForTest();
	int32 RemovedHookCalls = 0;
	int32 NoticesA = 0;
	int32 NoticesB = 0;
	bool bCallbackOK = true;
	FGameplayCueParameters CueParameters;
	CueParameters.RawMagnitude = 17.0f;
	CueParameters.Location = FVector(11.0, 13.0, 17.0);
	CueParameters.SourceObject = Fixture.Owner;
	const auto CheckPublicationResult = [&](const FGGYGOAvatarBindingResult& Result,
		const FGGYGOAvatarBindingResult& Commit)
	{
		bool bOK = TestEqual(TEXT("Real publication succeeded"), Result.Outcome, EGGYGOAvatarBindingOutcome::Succeeded);
		bOK &= TestEqual(TEXT("Real publication reason None"), Result.Reason, EGGYGOAvatarBindingReason::None);
		bOK &= TestTrue(TEXT("Publication preserves historical commit"), Result.bCommitted);
		bOK &= TestTrue(TEXT("Publication preserves original operation"), Result.Operation.HasSameIdentity(Commit.Operation));
		bOK &= TestTrue(TEXT("Publication preserves original Before"), SameContextValue(Result.Before, Commit.Before));
		bOK &= TestTrue(TEXT("Publication preserves original commit"), Result.CommittedContext.HasSameContext(Commit.CommittedContext));
		return bOK;
	};
	const auto CheckUnchangedA = [&]()
	{
		EGGYGOAvatarBindingReason Reason;
		bool bOK = TestTrue(TEXT("Cue call preserves exact original Binding and LastWrite"),
			Fixture.ASC->GetAvatarBindingContext().HasSameContext(CommitA.CommittedContext));
		bOK &= TestEqual(TEXT("Original context still validates"),
			Fixture.ASC->CheckAvatarBindingContext(CommitA.CommittedContext, Reason), EGGYGOAvatarBindingOutcome::Succeeded);
		bOK &= TestEqual(TEXT("Original validation reason None"), Reason, EGGYGOAvatarBindingReason::None);
		bOK &= CheckActual(*this, Fixture, Fixture.Owner, Fixture.AvatarA);
		return bOK;
	};
	const auto CheckConfiguration = [&](const UGameplayCueSet& Set)
	{
		const FCueConfigurationSnapshot After = SnapshotConfiguration(Set);
		bool bOK = TestTrue(TEXT("Cue cleanup leaves valid data and routes"), After.bValid);
		bOK &= TestTrue(TEXT("Other Cue Tag/path/loaded class/parent configuration is unchanged"),
			After.DataRows == BeforeConfiguration.DataRows);
		bOK &= TestTrue(TEXT("All effective Cue routes return to original semantics"),
			After.ValidRouteRows == BeforeConfiguration.ValidRouteRows);
		return bOK;
	};

	// All captured storage precedes the guard. Close retires callbacks before native cleanup.
	struct FCueRemovalTestResources
	{
		FAutomationTestBase& Test;
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
		TWeakObjectPtr<UGameplayCueSet> Set;
		TWeakObjectPtr<UGGYGOAvatarBindingCueRemovalTestNotify> Probe;
		FGameplayTag Tag;
		FSoftObjectPath Path;
		TFunction<bool(const UGameplayCueSet&)> CheckConfiguration;
		FDelegateHandle NoticeHandle;
		bool bOwnMapping = false;
		bool bOwnCue = false;
		bool bClosed = false;
		bool bCloseOK = true;
		bool Close()
		{
			if (bClosed) { return bCloseOK; }
			bClosed = true;
			if (UGGYGOAvatarBindingCueRemovalTestNotify* OriginalProbe = Probe.Get())
			{
				OriginalProbe->DisarmRemovedHookForTest();
			}
			if (UGGYGOAbilitySystemComponent* OriginalASC = ASC.Get())
			{
				if (NoticeHandle.IsValid()) { OriginalASC->OnAvatarBindingNotice().Remove(NoticeHandle); }
				if (bOwnCue) { OriginalASC->RemoveGameplayCue(Tag); }
			}
			else if (bOwnCue) { bCloseOK &= Test.TestTrue(TEXT("Original ASC must survive owned Cue cleanup"), false); }
			UGameplayCueSet* OriginalSet = Set.Get();
			if (!OriginalSet) { bCloseOK &= Test.TestTrue(TEXT("Original CueSet must survive mapping cleanup"), false); return bCloseOK; }
			if (bOwnMapping)
			{
				int32 OwnEntries = 0;
				bool bOnlyOriginalPath = true;
				for (const FGameplayCueNotifyData& Data : OriginalSet->GameplayCueData)
				{
					if (Data.GameplayCueTag == Tag)
					{
						++OwnEntries;
						bOnlyOriginalPath &= Data.GameplayCueNotifyObj == Path;
					}
				}
				bCloseOK &= Test.TestTrue(TEXT("Cleanup never removes a foreign mapping on the original Tag"),
					OwnEntries <= 1 && bOnlyOriginalPath);
				if (OwnEntries == 1 && bOnlyOriginalPath)
				{
					OriginalSet->RemoveCuesByTags(FGameplayTagContainer(Tag));
				}
				for (const FGameplayCueNotifyData& Data : OriginalSet->GameplayCueData)
				{
					bCloseOK &= Test.TestFalse(TEXT("Owned direct Cue mapping is retired"), Data.GameplayCueTag == Tag);
				}
			}
			bCloseOK &= CheckConfiguration(*OriginalSet);
			return bCloseOK;
		}
		~FCueRemovalTestResources() { Close(); }
	} Resources{*this, TWeakObjectPtr<UGGYGOAbilitySystemComponent>(Fixture.ASC),
		TWeakObjectPtr<UGameplayCueSet>(CueSet), TWeakObjectPtr<UGGYGOAvatarBindingCueRemovalTestNotify>(Notify),
		CueTag, NotifyPath, CheckConfiguration};
	Resources.bOwnMapping = true; // Cleanup covers a partially failed public registration.
	CueSet->AddCues(TArray<FGameplayCueReferencePair>{FGameplayCueReferencePair(CueTag, NotifyPath)});
	{
		const int32* Index = CueSet->GameplayCueDataMap.Find(CueTag);
		if (!TestTrue(TEXT("Public CueSet registration selects the exact native Notify"),
			Index && CueSet->GameplayCueData.IsValidIndex(*Index)
			&& CueSet->GameplayCueData[*Index].GameplayCueTag == CueTag
			&& CueSet->GameplayCueData[*Index].GameplayCueNotifyObj == NotifyPath)) { return false; }
	}
	SeedCommit = Fixture.ASC->TryBootstrapAvatarActorInfoTransaction(InitA, SeedReceipt);
	if (!CheckCommit(*this, Fixture, SeedCommit, SeedReceipt, {}, EGGYGOAvatarBindingNoticeKind::Initialized,
		Fixture.Owner, Fixture.AvatarA)) { return false; }
	CommitA = Fixture.ASC->TryExecuteAvatarActorInfoTransaction(
		Fixture.Request(EGGYGOAvatarBindingKind::Refresh, SeedCommit.CommittedContext), ReceiptA);
	if (!CheckCommit(*this, Fixture, CommitA, ReceiptA, SeedCommit.CommittedContext,
		EGGYGOAvatarBindingNoticeKind::Refreshed, Fixture.Owner, Fixture.AvatarA, true)
		|| !TestFalse(TEXT("Real Refresh makes the original issued Context stale"),
			SeedCommit.CommittedContext.HasSameContext(CommitA.CommittedContext))) { return false; }
	Resources.NoticeHandle = Fixture.ASC->OnAvatarBindingNotice().AddLambda(
		[&](const FGGYGOAvatarBindingPublicationReceipt&, const FGGYGOAvatarBindingNotice& Notice)
		{
			if (Notice.After.HasSameContext(CommitA.CommittedContext))
			{
				++NoticesA;
				bCallbackOK &= TestEqual(TEXT("A publishes exactly once"), NoticesA, 1);
			}
			else if (Notice.After.HasSameContext(CommitB.CommittedContext))
			{
				++NoticesB;
				bCallbackOK &= TestEqual(TEXT("Legal B publishes exactly once"), NoticesB, 1);
			}
			else { bCallbackOK &= TestTrue(TEXT("Only current A or legal B can notify"), false); }
		});
	if (!TestTrue(TEXT("Real notice subscriber installed"), Resources.NoticeHandle.IsValid())) { return false; }
	const FGGYGOAvatarBindingResult PublishedA = Fixture.ASC->PublishAvatarBindingNotice(ReceiptA, InitA.IsRequestContextCurrent);
	if (!bCallbackOK || !CheckPublicationResult(PublishedA, CommitA)
		|| !TestTrue(TEXT("A is published before Cue cleanup"),
			Fixture.ASC->IsAvatarBindingPublicationContextCurrent(CommitA.CommittedContext))) { return false; }
	Resources.bOwnCue = true;
	Fixture.ASC->AddGameplayCue(CueTag, CueParameters);
	if (!TestEqual(TEXT("Actual Add reaches native WhileActive once"), Notify->GetWhileActiveCallsForTest(), WhileActiveBefore + 1)
		|| !TestTrue(TEXT("Native WhileActive reports original A"),
			Notify->GetLastWhileActiveTargetForTest().HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(Fixture.AvatarA)))
		|| !TestEqual(TEXT("Actual Add owns one count of this Cue"), Fixture.ASC->GetTagCount(CueTag), 1)
		|| !TestEqual(TEXT("Add has not emitted Removed"), Notify->GetRemovedCallsForTest(), RemovedBefore)) { return false; }

	const FGGYGOAvatarBindingResult MissingQuery = Fixture.ASC->TryRemoveAvatarBindingGameplayCues(
		CommitA.CommittedContext, TFunction<bool()>{});
	if (!CheckFailure(*this, MissingQuery, EGGYGOAvatarBindingOutcome::Rejected,
		EGGYGOAvatarBindingReason::MissingContextQuery, false)
		|| !TestEqual(TEXT("Missing query does not invoke native Removed"), Notify->GetRemovedCallsForTest(), RemovedBefore)
		|| !TestEqual(TEXT("Missing query leaves the real Cue count"), Fixture.ASC->GetTagCount(CueTag), 1)) { return false; }
	const FGGYGOAvatarBindingResult OldContext = Fixture.ASC->TryRemoveAvatarBindingGameplayCues(
		SeedCommit.CommittedContext, InitA.IsRequestContextCurrent);
	if (!CheckFailure(*this, OldContext, EGGYGOAvatarBindingOutcome::Stale,
		EGGYGOAvatarBindingReason::ExpectedContextMismatch, false)
		|| !TestEqual(TEXT("Old Context does not invoke native Removed"), Notify->GetRemovedCallsForTest(), RemovedBefore)
		|| !TestEqual(TEXT("Old Context leaves the real Cue count"), Fixture.ASC->GetTagCount(CueTag), 1)
		|| !CheckUnchangedA()
		|| !TestTrue(TEXT("Admission rejection retains A publication"),
			Fixture.ASC->IsAvatarBindingPublicationContextCurrent(CommitA.CommittedContext))) { return false; }

	Notify->ArmRemovedHookForTest(FGGYGOAvatarBindingCueRemovalHook::CreateLambda(
		[&](AActor* Target, const FGameplayCueParameters& NativeParameters)
		{
			++RemovedHookCalls;
			bCallbackOK &= TestEqual(TEXT("Real native Removed hook occurs once"), RemovedHookCalls, 1);
			bCallbackOK &= TestTrue(TEXT("Removed routes to exact original A"), Target == Fixture.AvatarA);
			bCallbackOK &= TestTrue(TEXT("Removed retains original native Tag"), NativeParameters.OriginalTag == CueTag);
			bCallbackOK &= TestTrue(TEXT("Removed retains matched native Tag"), NativeParameters.MatchedTagName == CueTag);
			bCallbackOK &= TestEqual(TEXT("Removed retains actual Add magnitude"), NativeParameters.RawMagnitude, CueParameters.RawMagnitude);
			bCallbackOK &= TestTrue(TEXT("Removed retains actual Add location"), NativeParameters.Location == CueParameters.Location);
			bCallbackOK &= TestTrue(TEXT("Removed retains original source object"), NativeParameters.SourceObject.Get() == Fixture.Owner);
			bCallbackOK &= TestTrue(TEXT("Real Removed executes inside original Busy"), Fixture.ASC->IsAvatarBindingNativeWriteBusy());
			bCallbackOK &= CheckUnchangedA();
			NestedReceipt = ReceiptA;
			NestedBind = Fixture.ASC->TryExecuteAvatarActorInfoTransaction(
				Fixture.Request(EGGYGOAvatarBindingKind::Init, CommitA.CommittedContext, Fixture.AvatarB), NestedReceipt);
			bCallbackOK &= CheckFailure(*this, NestedBind,
				EGGYGOAvatarBindingOutcome::Busy, EGGYGOAvatarBindingReason::NativeWriteBusy, false);
			bCallbackOK &= CheckEmptyPublication(*this, NestedReceipt, ReceiptA);
			bCallbackOK &= TestTrue(TEXT("Failed B entry preserves original Busy"), Fixture.ASC->IsAvatarBindingNativeWriteBusy());
			bCallbackOK &= CheckUnchangedA();
		}));
	const FGGYGOAvatarBindingResult Removed = Fixture.ASC->TryRemoveAvatarBindingGameplayCues(
		CommitA.CommittedContext, InitA.IsRequestContextCurrent);
	if (!bCallbackOK || !TestEqual(TEXT("Cue call synchronously succeeds"), Removed.Outcome, EGGYGOAvatarBindingOutcome::Succeeded)
		|| !TestEqual(TEXT("Cue call reason None"), Removed.Reason, EGGYGOAvatarBindingReason::None)
		|| !TestTrue(TEXT("Cue call owns ASC-issued operation"), Removed.Operation.HasIssuedIdentity() && Removed.Operation.Issuer.Get() == Fixture.ASC)
		|| !TestTrue(TEXT("Cue call retains exact original Before"), Removed.Before.HasSameContext(CommitA.CommittedContext))
		|| !TestFalse(TEXT("Cue call does not commit binding"), Removed.bCommitted)
		|| !TestTrue(TEXT("Cue call has no committed context"), IsEmptyContext(Removed.CommittedContext))
		|| !TestEqual(TEXT("Exactly one actual OnRemove was routed"), Notify->GetRemovedCallsForTest(), RemovedBefore + 1)
		|| !TestEqual(TEXT("Actual OnRemove executed the hook"), RemovedHookCalls, 1)
		|| !TestTrue(TEXT("Native Removed records original A"),
			Notify->GetLastRemovedTargetForTest().HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(Fixture.AvatarA)))
		|| !TestFalse(TEXT("One-shot Notify hook does not retain captured storage"), Notify->IsRemovedHookArmedForTest())
		|| !CheckUnchangedA()
		|| !TestFalse(TEXT("Full native return releases Busy"), Fixture.ASC->IsAvatarBindingNativeWriteBusy())
		|| !TestFalse(TEXT("Matching A publication permission is closed"),
			Fixture.ASC->IsAvatarBindingPublicationContextCurrent(CommitA.CommittedContext))
		|| !TestEqual(TEXT("Cue cleanup has no extra A notice"), NoticesA, 1)
		|| !TestEqual(TEXT("Rejected B entry has no notice"), NoticesB, 0)
		|| !TestEqual(TEXT("Only this fixture's Cue count is removed"), Fixture.ASC->GetTagCount(CueTag), 0)) { return false; }

	CommitB = Fixture.ASC->TryExecuteAvatarActorInfoTransaction(
		Fixture.Request(EGGYGOAvatarBindingKind::Init, CommitA.CommittedContext, Fixture.AvatarB), ReceiptB);
	if (!CheckCommit(*this, Fixture, CommitB, ReceiptB, CommitA.CommittedContext,
		EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, Fixture.AvatarB)
		|| !CheckActual(*this, Fixture, Fixture.Owner, Fixture.AvatarB)) { return false; }
	const FGGYGOAvatarBindingResult PublishedB = Fixture.ASC->PublishAvatarBindingNotice(ReceiptB, InitA.IsRequestContextCurrent);
	bool bOK = bCallbackOK;
	bOK &= CheckPublicationResult(PublishedB, CommitB);
	bOK &= TestEqual(TEXT("Legal B generates exactly one notice"), NoticesB, 1);
	bOK &= TestEqual(TEXT("A has no repeated notice"), NoticesA, 1);
	bOK &= TestTrue(TEXT("B publication remains current"), Fixture.ASC->IsAvatarBindingPublicationContextCurrent(CommitB.CommittedContext));
	bOK &= TestFalse(TEXT("Legal B returns outside native Busy"), Fixture.ASC->IsAvatarBindingNativeWriteBusy());
	bOK &= CheckActual(*this, Fixture, Fixture.Owner, Fixture.AvatarB);
	bOK &= CheckHistory(*this, SeedReceipt, SeedCommit, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, Fixture.AvatarA);
	bOK &= CheckHistory(*this, ReceiptA, CommitA, EGGYGOAvatarBindingNoticeKind::Refreshed, Fixture.Owner, Fixture.AvatarA);
	bOK &= CheckHistory(*this, ReceiptB, CommitB, EGGYGOAvatarBindingNoticeKind::Initialized, Fixture.Owner, Fixture.AvatarB);
	bOK &= Resources.Close();
	bOK &= TestEqual(TEXT("Idempotent cleanup cannot emit another Removed"), Notify->GetRemovedCallsForTest(), RemovedBefore + 1);
	return bOK;
}

#endif // WITH_DEV_AUTOMATION_TESTS

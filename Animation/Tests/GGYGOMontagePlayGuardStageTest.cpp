/** @file GGYGOMontagePlayGuardStageTest.cpp @brief Real scope and native montage stage query tests. */
#if WITH_DEV_AUTOMATION_TESTS

#include "AbilitySystem/Tests/GGYGOMontageTaskTestTypes.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimMontage.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Misc/AutomationTest.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	using EStage = EGGYGOMontagePlayGuardNativeStage;
	using EOutcome = EGGYGOMontagePlayGuardOutcome;
	using FScope = FGGYGOMontagePlayGuardScope;
	using FResult = FGGYGOMontagePlayGuardResult;

	struct FStageTestWorld
	{
		UEngine* Engine = GEngine;
		UWorld* World = nullptr;

		FStageTestWorld()
		{
			if (!Engine) { return; }
			UWorld::InitializationValues Init;
			Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(false)
				.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Init);
			if (World) { Engine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World); }
		}

		~FStageTestWorld()
		{
			if (World)
			{
				// Actor/component teardown must still be able to resolve this world's context.
				World->DestroyWorld(false);
				Engine->DestroyWorldContext(World);
				if (UPackage* Package = World->GetPackage()) { Package->SetDirtyFlag(false); }
			}
		}
	};

	/** Owns a separate world fixture; it never installs Task hooks or overrides native play. */
	struct FStageTestFixture
	{
		ACharacter* Avatar = nullptr;
		USkeletalMeshComponent* Mesh = nullptr;
		TStrongObjectPtr<UGGYGOGuardedMontageStartedTestAnimInstance> Guard{nullptr};
		TStrongObjectPtr<UGGYGOMontageStartedDiagnosticASC> Coordinator{nullptr};
		TStrongObjectPtr<UGGYGOMontageStartedDiagnosticASC> OtherCoordinator{nullptr};
		TStrongObjectPtr<UGGYGOMontageStartedDiagnosticObserver> ObserverA{nullptr};
		TStrongObjectPtr<UGGYGOMontageStartedDiagnosticObserver> ObserverB{nullptr};
		TStrongObjectPtr<UAnimMontage> MontageA{nullptr};
		TStrongObjectPtr<UAnimMontage> MontageB{nullptr};
		TStrongObjectPtr<UAnimMontage> ZeroLengthMontage{nullptr};
		int32 PredicateCalls = 0;
		bool bCallerContextCurrent = true;

		~FStageTestFixture()
		{
			if (IsValid(ObserverA.Get())) { ObserverA->DisarmForTest(); }
			if (IsValid(ObserverB.Get())) { ObserverB->DisarmForTest(); }
			// All caller scopes and their captures have already exited. Only this fixture plays here.
			if (IsValid(Guard.Get()) && IsValid(Mesh) && Mesh->GetAnimInstance() == Guard.Get()
				&& Guard->GetSkelMeshComponent() == Mesh && Guard->GetOwningActor() == Avatar)
			{
				Guard->Montage_Stop(0.0f);
				Guard->UninitializeAnimation();
			}
		}

		UAnimMontage* MakeMontage(USkeleton* Skeleton, bool bZeroLength)
		{
			UAnimMontage* Montage = NewObject<UAnimMontage>(Avatar);
			Montage->SetSkeleton(Skeleton);
			if (Montage->SlotAnimTracks.Num() != 1) { return nullptr; }
			FSlotAnimationTrack& Slot = Montage->SlotAnimTracks[0];
			Slot.SlotName = TEXT("DefaultSlot");
			UAnimComposite* SegmentAsset = NewObject<UAnimComposite>(Montage);
			SegmentAsset->SetSkeleton(Skeleton);
			SegmentAsset->SetCompositeLength(1.0f);
			FAnimSegment Segment;
			Segment.SetAnimReference(SegmentAsset, true);
			Slot.AnimTrack.AnimSegments.Add(Segment);
			// Deliberate input to the native length gate, with all object/skeleton/slot inputs valid.
			Montage->SetCompositeLength(bZeroLength ? 0.0f : 1.0f);
			const FAnimSegment& Validated = Slot.AnimTrack.AnimSegments[0];
			const UAnimSequenceBase* Reference = Validated.GetAnimReference().Get();
			return Montage->IsValidSlot(Slot.SlotName) && !Montage->GetGroupName().IsNone()
				&& Validated.IsValid() && Reference && Reference->GetSkeleton() == Skeleton
				&& FMath::IsFinite(Validated.GetLength()) && Validated.GetLength() > 0.0f
				&& Validated.AnimStartTime >= 0.0f && Validated.AnimEndTime <= Reference->GetPlayLength()
				&& Montage->GetPlayLength() == (bZeroLength ? 0.0f : 1.0f) ? Montage : nullptr;
		}

		bool Initialize(FAutomationTestBase& Test, UWorld* World)
		{
			if (!Test.TestNotNull(TEXT("Prerequisite.World"), World)) { return false; }
			Avatar = World->SpawnActor<ACharacter>();
			if (!Test.TestTrue(TEXT("Prerequisite.AuthorityAvatar"), IsValid(Avatar) && Avatar->HasAuthority()))
			{
				return false;
			}
			USkeletalMesh* MeshAsset = LoadObject<USkeletalMesh>(nullptr,
				TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
			USkeleton* Skeleton = MeshAsset ? MeshAsset->GetSkeleton() : nullptr;
			FSkeletalMeshRenderData* RenderData = MeshAsset ? MeshAsset->GetResourceForRendering() : nullptr;
			if (!Test.TestTrue(TEXT("Prerequisite.RealMeshSkeleton"), Skeleton && RenderData
				&& !RenderData->LODRenderData.IsEmpty() && MeshAsset->GetRefSkeleton().GetRawBoneNum() > 0
				&& MeshAsset->GetRefSkeleton().GetNum() > 0 && Skeleton->IsCompatibleMesh(MeshAsset)))
			{
				return false;
			}
			Mesh = Avatar->GetMesh();
			if (!Test.TestTrue(TEXT("Prerequisite.RegisteredMesh"), Mesh && Mesh->IsRegistered())) { return false; }
			Mesh->SetComponentTickEnabled(false);
			Mesh->SetVisibility(false);
			Mesh->SetCastShadow(false);
			Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
			Mesh->bEnableUpdateRateOptimizations = true;
			Mesh->SetSkinnedAssetAndUpdate(MeshAsset);
			Mesh->SetAnimInstanceClass(UGGYGOGuardedMontageStartedTestAnimInstance::StaticClass());
			Guard.Reset(Cast<UGGYGOGuardedMontageStartedTestAnimInstance>(Mesh->GetAnimInstance()));
			if (!Test.TestTrue(TEXT("Prerequisite.ProductionGuardAndOwner"), Guard.Get()
				&& Guard->GetClass() == UGGYGOGuardedMontageStartedTestAnimInstance::StaticClass()
				&& Guard->GetSkelMeshComponent() == Mesh && Guard->GetOwningActor() == Avatar)) { return false; }
			MontageA.Reset(MakeMontage(Skeleton, false));
			MontageB.Reset(MakeMontage(Skeleton, false));
			ZeroLengthMontage.Reset(MakeMontage(Skeleton, true));
			if (!Test.TestTrue(TEXT("Prerequisite.RealMontagesAndZeroLengthInput"), MontageA.Get() && MontageB.Get()
				&& ZeroLengthMontage.Get() && MontageA.Get() != MontageB.Get()
				&& ZeroLengthMontage.Get() != MontageA.Get()
				&& MontageA->GetGroupName() == MontageB->GetGroupName()
				&& MontageA->GetGroupName() == ZeroLengthMontage->GetGroupName())) { return false; }
			Coordinator.Reset(NewObject<UGGYGOMontageStartedDiagnosticASC>(Avatar));
			OtherCoordinator.Reset(NewObject<UGGYGOMontageStartedDiagnosticASC>(Avatar));
			Coordinator->RegisterComponent();
			OtherCoordinator->RegisterComponent();
			Coordinator->InitAbilityActorInfo(Avatar, Avatar);
			OtherCoordinator->InitAbilityActorInfo(Avatar, Avatar);
			ObserverA.Reset(NewObject<UGGYGOMontageStartedDiagnosticObserver>(Avatar));
			ObserverB.Reset(NewObject<UGGYGOMontageStartedDiagnosticObserver>(Avatar));
			return Test.TestTrue(TEXT("Prerequisite.RealCoordinatorsAndObservers"), Coordinator.Get() != OtherCoordinator.Get()
				&& IsCoordinatorCurrent(Coordinator.Get()) && IsCoordinatorCurrent(OtherCoordinator.Get())
				&& ObserverA.Get() && ObserverB.Get());
		}

		bool IsCoordinatorCurrent(const UGGYGOMontageStartedDiagnosticASC* Caller) const
		{
			return IsValid(Caller) && IsValid(Avatar) && IsValid(Mesh) && IsValid(Guard.Get())
				&& Caller->AbilityActorInfo.IsValid() && Caller->AbilityActorInfo->OwnerActor.Get() == Avatar
				&& Caller->AbilityActorInfo->AvatarActor.Get() == Avatar
				&& Caller->AbilityActorInfo->SkeletalMeshComponent.Get() == Mesh
				&& Caller->AbilityActorInfo->GetAnimInstance() == Guard.Get()
				&& Mesh->GetAnimInstance() == Guard.Get() && Guard->GetOwningActor() == Avatar;
		}

		FGGYGOMontagePlayGuardRequest MakeRequest(UAnimMontage* Montage,
			UGGYGOMontageStartedDiagnosticASC* Caller)
		{
			FGGYGOMontagePlayGuardRequest Request;
			Request.OriginalAnimInstance = Guard.Get();
			Request.RequestedMontage = Montage;
			Request.CallerIdentity = Caller;
			Request.IsCallerContextCurrent = [this, Caller]()
			{
				++PredicateCalls; // Diagnostic count only; no production state changes or external calls.
				return bCallerContextCurrent && IsCoordinatorCurrent(Caller);
			};
			return Request;
		}
	};

	struct FStartedObservation
	{
		UGGYGOMontageStartedDiagnosticObserver* Observer;
		FStartedObservation(UGGYGOMontageStartedDiagnosticObserver* InObserver, UAnimInstance* Anim,
			UAnimMontage* Montage, TFunction<void()> Action) : Observer(InObserver)
		{
			Observer->ArmForTest(Anim, Montage, MoveTemp(Action));
		}
		~FStartedObservation() { Observer->DisarmForTest(); }
	};

	bool SameResult(const FResult& A, const FResult& B)
	{
		return A.Identity.OriginalAnimInstance == B.Identity.OriginalAnimInstance
			&& A.Identity.LifecycleGeneration == B.Identity.LifecycleGeneration && A.Identity.CallId == B.Identity.CallId
			&& A.Identity.CreatedInstanceId == B.Identity.CreatedInstanceId && A.Outcome == B.Outcome
			&& A.NativeStage == B.NativeStage && A.SupersedingCallId == B.SupersedingCallId
			&& A.GuardedNativeReturnValue == B.GuardedNativeReturnValue && A.CallerReturnValue == B.CallerReturnValue;
	}

	bool CheckStage(FAutomationTestBase& Test, const FString& Label, FStageTestFixture& Fixture,
		const UObject* Caller, bool bExpectedAvailable, EStage ExpectedStage, bool bExpectedCompleted,
		const FScope* ObservedScope = nullptr)
	{
		const int32 CallsBefore = Fixture.PredicateCalls;
		const FResult Before = ObservedScope ? ObservedScope->GetResult() : FResult();
		// Both unavailable outputs must actively overwrite these non-default sentinels.
		EStage Stage = EStage::Returned;
		bool bCompleted = true;
		const bool bAvailable = Fixture.Guard->TryGetCurrentMontagePlayGuardStage(Caller, Stage, bCompleted);
		bool bPassed = Test.TestEqual(Label + TEXT(".Available"), bAvailable, bExpectedAvailable);
		bPassed &= Test.TestEqual(Label + TEXT(".Stage"), static_cast<uint8>(Stage), static_cast<uint8>(ExpectedStage));
		bPassed &= Test.TestEqual(Label + TEXT(".Completed"), bCompleted, bExpectedCompleted);
		bPassed &= Test.TestEqual(Label + TEXT(".NoExternalPredicate"), Fixture.PredicateCalls, CallsBefore);
		if (ObservedScope)
		{
			bPassed &= Test.TestTrue(Label + TEXT(".ResultUnchanged"), SameResult(Before, ObservedScope->GetResult()));
		}
		return bPassed;
	}

	int32 ReadActiveId(UGGYGOMontageGuardAnimInstance* Guard, UAnimMontage* Montage)
	{
		const FAnimMontageInstance* Instance = Guard->GetActiveInstanceForMontage(Montage);
		return Instance && Instance->IsActive() && Instance->IsPlaying() ? Instance->GetInstanceID() : INDEX_NONE;
	}

	bool IsExactPlaying(UGGYGOMontageGuardAnimInstance* Guard, int32 Id, UAnimMontage* Montage)
	{
		const FAnimMontageInstance* Instance = Guard->GetMontageInstanceForID(Id);
		return Instance && Instance->Montage == Montage && Instance->IsActive() && Instance->IsPlaying();
	}

	bool RunSingleCall(FAutomationTestBase& Test, FStageTestFixture& Fixture, const FString& Label,
		FResult& OutResult)
	{
		bool bPassed = CheckStage(Test, Label + TEXT(".EmptyBefore"), Fixture, Fixture.Coordinator.Get(),
			false, EStage::NotEntered, false);
		int32 StartedCount = 0;
		int32 StartedId = INDEX_NONE;
		{
			FScope Scope(Fixture.MakeRequest(Fixture.MontageA.Get(), Fixture.Coordinator.Get()));
			bPassed &= CheckStage(Test, Label + TEXT(".Constructed"), Fixture, Fixture.Coordinator.Get(),
				true, EStage::NotEntered, false, &Scope);
			bPassed &= CheckStage(Test, Label + TEXT(".NullCaller"), Fixture, nullptr,
				false, EStage::NotEntered, false, &Scope);
			bPassed &= CheckStage(Test, Label + TEXT(".WrongCaller"), Fixture, Fixture.OtherCoordinator.Get(),
				false, EStage::NotEntered, false, &Scope);
			const bool bCanExecute = Scope.CanExecute();
			bPassed &= Test.TestTrue(Label + TEXT(".NativeEntryAuthorized"), bCanExecute);
			if (!bCanExecute) { Scope.Complete(0.0f); return false; }
			{
				FStartedObservation Observation(Fixture.ObserverA.Get(), Fixture.Guard.Get(), Fixture.MontageA.Get(), [&]()
				{
					++StartedCount;
					StartedId = ReadActiveId(Fixture.Guard.Get(), Fixture.MontageA.Get());
					bPassed &= Test.TestTrue(Label + TEXT(".Started.RealPlayingInstance"), StartedId != INDEX_NONE);
					bPassed &= CheckStage(Test, Label + TEXT(".Started"), Fixture, Fixture.Coordinator.Get(),
						true, EStage::Executing, false, &Scope);
				});
				const float Duration = Fixture.Guard->Montage_Play(Fixture.MontageA.Get(), 1.0f,
					EMontagePlayReturnType::MontageLength, 0.0f, true);
				bPassed &= Test.TestTrue(Label + TEXT(".NativePositive"), FMath::IsFinite(Duration) && Duration > 0.0f);
				bPassed &= Test.TestEqual(Label + TEXT(".Started.ExactlyOnce"), StartedCount, 1);
				bPassed &= CheckStage(Test, Label + TEXT(".ReturnedBeforeComplete"), Fixture, Fixture.Coordinator.Get(),
					true, EStage::Returned, false, &Scope);
				// The stage query must not reinterpret the caller's external validity predicate.
				Fixture.bCallerContextCurrent = false;
				bPassed &= CheckStage(Test, Label + TEXT(".ExternalContextFalse"), Fixture, Fixture.Coordinator.Get(),
					true, EStage::Returned, false, &Scope);
				Fixture.bCallerContextCurrent = true;
				OutResult = Scope.Complete(Duration);
			}
			bPassed &= CheckStage(Test, Label + TEXT(".Completed"), Fixture, Fixture.Coordinator.Get(),
				true, EStage::Returned, true, &Scope);
			bPassed &= CheckStage(Test, Label + TEXT(".Completed.RepeatedQuery"), Fixture, Fixture.Coordinator.Get(),
				true, EStage::Returned, true, &Scope);
			bPassed &= Test.TestTrue(Label + TEXT(".Accepted"), OutResult.Outcome == EOutcome::Accepted);
			bPassed &= Test.TestEqual(Label + TEXT(".ExactIssuedInstance"), OutResult.Identity.CreatedInstanceId, StartedId);
			bPassed &= Test.TestTrue(Label + TEXT(".ExactInstanceStillPlaying"),
				IsExactPlaying(Fixture.Guard.Get(), OutResult.Identity.CreatedInstanceId, Fixture.MontageA.Get()));
		}
		bPassed &= CheckStage(Test, Label + TEXT(".Destroyed"), Fixture, Fixture.Coordinator.Get(),
			false, EStage::NotEntered, false);
		return bPassed;
	}

	bool RunNestedCall(FAutomationTestBase& Test, FStageTestFixture& Fixture, bool bFailChild)
	{
		const FString Label = bFailChild ? TEXT("FailedB") : TEXT("AcceptedB");
		bool bPassed = true;
		int32 StartedA = 0;
		int32 StartedB = 0;
		int32 AId = INDEX_NONE;
		FResult ResultB;
		UAnimMontage* ChildMontage = bFailChild ? Fixture.ZeroLengthMontage.Get() : Fixture.MontageB.Get();
		{
			FScope A(Fixture.MakeRequest(Fixture.MontageA.Get(), Fixture.Coordinator.Get()));
			const bool bCanExecuteA = A.CanExecute();
			bPassed &= Test.TestTrue(Label + TEXT(".A.NativeEntryAuthorized"), bCanExecuteA);
			if (!bCanExecuteA) { A.Complete(0.0f); return false; }
			{
				FStartedObservation ObserveA(Fixture.ObserverA.Get(), Fixture.Guard.Get(), Fixture.MontageA.Get(), [&]()
				{
					++StartedA;
					AId = ReadActiveId(Fixture.Guard.Get(), Fixture.MontageA.Get());
					bPassed &= Test.TestTrue(Label + TEXT(".A.Started.RealPlayingInstance"), AId != INDEX_NONE);
					bPassed &= CheckStage(Test, Label + TEXT(".A.Started"), Fixture, Fixture.Coordinator.Get(),
						true, EStage::Executing, false, &A);
					if (AId == INDEX_NONE) { return; }
					{
						FScope B(Fixture.MakeRequest(ChildMontage, Fixture.Coordinator.Get()));
						bPassed &= CheckStage(Test, Label + TEXT(".B.ConstructedIsInnermost"), Fixture,
							Fixture.Coordinator.Get(), true, EStage::NotEntered, false, &B);
						const bool bCanExecuteB = B.CanExecute();
						bPassed &= Test.TestTrue(Label + TEXT(".B.NativeEntryAuthorized"), bCanExecuteB);
						if (!bCanExecuteB) { B.Complete(0.0f); return; }
						float BDuration = 0.0f;
						{
							FStartedObservation ObserveB(Fixture.ObserverB.Get(), Fixture.Guard.Get(), ChildMontage, [&]()
							{
								++StartedB;
								bPassed &= Test.TestTrue(Label + TEXT(".B.Started.RealPlayingInstance"),
									ReadActiveId(Fixture.Guard.Get(), ChildMontage) != INDEX_NONE);
								bPassed &= CheckStage(Test, Label + TEXT(".B.Started"), Fixture,
									Fixture.Coordinator.Get(), true, EStage::Executing, false, &B);
							});
							BDuration = Fixture.Guard->Montage_Play(ChildMontage, 1.0f,
								EMontagePlayReturnType::MontageLength, 0.0f, true);
						}
						bPassed &= Test.TestEqual(Label + TEXT(".B.StartedCount"), StartedB, bFailChild ? 0 : 1);
						bPassed &= Test.TestTrue(Label + TEXT(".B.NativeReturn"), FMath::IsFinite(BDuration)
							&& (bFailChild ? BDuration == 0.0f : BDuration > 0.0f));
						bPassed &= CheckStage(Test, Label + TEXT(".B.ReturnedIsInnermost"), Fixture,
							Fixture.Coordinator.Get(), true, EStage::Returned, false, &B);
						bPassed &= Test.TestTrue(Label + TEXT(".A.StillInNativeFrame"), A.GetResult().NativeStage == EStage::Executing);
						ResultB = B.Complete(BDuration);
						bPassed &= CheckStage(Test, Label + TEXT(".B.CompletedIsInnermost"), Fixture,
							Fixture.Coordinator.Get(), true, EStage::Returned, true, &B);
						bPassed &= Test.TestTrue(Label + TEXT(".B.Outcome"),
							ResultB.Outcome == (bFailChild ? EOutcome::Failed : EOutcome::Accepted));
						bPassed &= Test.TestEqual(Label + TEXT(".A.TakeoverOnlyOnAcceptance"), A.GetResult().SupersedingCallId,
							bFailChild ? uint64(0) : ResultB.Identity.CallId);
						if (bFailChild)
						{
							bPassed &= Test.TestEqual(Label + TEXT(".B.NoCreatedInstance"), ResultB.Identity.CreatedInstanceId, INDEX_NONE);
							bPassed &= Test.TestTrue(Label + TEXT(".A.InstanceSurvivesFailure"),
								IsExactPlaying(Fixture.Guard.Get(), AId, Fixture.MontageA.Get()));
						}
					}
					bPassed &= CheckStage(Test, Label + TEXT(".B.DestroyedRestoresA"), Fixture,
						Fixture.Coordinator.Get(), true, EStage::Executing, false, &A);
				});
				const float ADuration = Fixture.Guard->Montage_Play(Fixture.MontageA.Get(), 1.0f,
					EMontagePlayReturnType::MontageLength, 0.0f, true);
				bPassed &= Test.TestEqual(Label + TEXT(".A.StartedExactlyOnce"), StartedA, 1);
				bPassed &= CheckStage(Test, Label + TEXT(".A.Returned"), Fixture, Fixture.Coordinator.Get(),
					true, EStage::Returned, false, &A);
				const FResult ResultA = A.Complete(ADuration);
				bPassed &= Test.TestTrue(Label + TEXT(".A.FinalOutcome"),
					ResultA.Outcome == (bFailChild ? EOutcome::Accepted : EOutcome::Superseded));
				bPassed &= Test.TestTrue(Label + TEXT(".A.PublicReturn"), FMath::IsFinite(ADuration)
					&& (bFailChild ? ADuration > 0.0f : ADuration == 0.0f));
				bPassed &= Test.TestEqual(Label + TEXT(".A.FinalTakeover"), ResultA.SupersedingCallId,
					bFailChild ? uint64(0) : ResultB.Identity.CallId);
				bPassed &= CheckStage(Test, Label + TEXT(".A.Completed"), Fixture, Fixture.Coordinator.Get(),
					true, EStage::Returned, true, &A);
			}
		}
		bPassed &= CheckStage(Test, Label + TEXT(".AllDestroyed"), Fixture, Fixture.Coordinator.Get(),
			false, EStage::NotEntered, false);
		return bPassed;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageGuardStageSingleCallTest,
	"GGYGO.Animation.MontageGuard.StageQuery.SingleCall", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMontageGuardStageSingleCallTest::RunTest(const FString& Parameters)
{
	FStageTestWorld World;
	FStageTestFixture Fixture;
	if (!Fixture.Initialize(*this, World.World)) { return false; }
	FResult Result;
	return RunSingleCall(*this, Fixture, TEXT("Single"), Result);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageGuardStageAcceptedNestedTest,
	"GGYGO.Animation.MontageGuard.StageQuery.AcceptedNested", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMontageGuardStageAcceptedNestedTest::RunTest(const FString& Parameters)
{
	FStageTestWorld World;
	FStageTestFixture Fixture;
	if (!Fixture.Initialize(*this, World.World)) { return false; }
	return RunNestedCall(*this, Fixture, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageGuardStageFailedNestedTest,
	"GGYGO.Animation.MontageGuard.StageQuery.FailedNested", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMontageGuardStageFailedNestedTest::RunTest(const FString& Parameters)
{
	FStageTestWorld World;
	FStageTestFixture Fixture;
	if (!Fixture.Initialize(*this, World.World)) { return false; }
	return RunNestedCall(*this, Fixture, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageGuardStageDifferentCoordinatorTest,
	"GGYGO.Animation.MontageGuard.StageQuery.DifferentCoordinator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMontageGuardStageDifferentCoordinatorTest::RunTest(const FString& Parameters)
{
	FStageTestWorld World;
	FStageTestFixture Fixture;
	if (!Fixture.Initialize(*this, World.World)) { return false; }
	bool bPassed = true;
	int32 StartedA = 0;
	{
		FScope A(Fixture.MakeRequest(Fixture.MontageA.Get(), Fixture.Coordinator.Get()));
		const bool bCanExecuteA = A.CanExecute();
		bPassed &= TestTrue(TEXT("Foreign.A.NativeEntryAuthorized"), bCanExecuteA);
		if (!bCanExecuteA) { A.Complete(0.0f); return false; }
		{
			FStartedObservation Observation(Fixture.ObserverA.Get(), Fixture.Guard.Get(), Fixture.MontageA.Get(), [&]()
			{
				++StartedA;
				const int32 AId = ReadActiveId(Fixture.Guard.Get(), Fixture.MontageA.Get());
				bPassed &= TestTrue(TEXT("Foreign.A.RealPlayingInstance"), AId != INDEX_NONE);
				bPassed &= CheckStage(*this, TEXT("Foreign.A.Started"), Fixture, Fixture.Coordinator.Get(),
					true, EStage::Executing, false, &A);
				if (AId == INDEX_NONE) { return; }
				{
					FScope B(Fixture.MakeRequest(Fixture.MontageB.Get(), Fixture.OtherCoordinator.Get()));
					bPassed &= TestTrue(TEXT("Foreign.B.IssuedRegistration"), B.GetResult().Identity.CallId != 0
						&& B.GetResult().Identity.CallId != A.GetResult().Identity.CallId);
					bPassed &= TestFalse(TEXT("Foreign.B.NativeEntryRejected"), B.CanExecute());
					bPassed &= CheckStage(*this, TEXT("Foreign.B.CannotLookPastToA"), Fixture, Fixture.Coordinator.Get(),
						false, EStage::NotEntered, false, &B);
					bPassed &= CheckStage(*this, TEXT("Foreign.B.OwnRejectedScope"), Fixture, Fixture.OtherCoordinator.Get(),
						true, EStage::NotEntered, false, &B);
					const FResult ResultB = B.Complete(0.0f);
					bPassed &= TestTrue(TEXT("Foreign.B.UnsupportedNotNativeFailure"), ResultB.Outcome == EOutcome::Unsupported);
					bPassed &= TestEqual(TEXT("Foreign.B.NoCreatedInstance"), ResultB.Identity.CreatedInstanceId, INDEX_NONE);
					bPassed &= CheckStage(*this, TEXT("Foreign.B.CompletedCannotLookPastToA"), Fixture,
						Fixture.Coordinator.Get(), false, EStage::NotEntered, false, &B);
					bPassed &= CheckStage(*this, TEXT("Foreign.B.OwnCompletedScope"), Fixture,
						Fixture.OtherCoordinator.Get(), true, EStage::NotEntered, true, &B);
					bPassed &= TestEqual(TEXT("Foreign.A.NoTakeover"), A.GetResult().SupersedingCallId, uint64(0));
					bPassed &= TestTrue(TEXT("Foreign.A.InstanceUnaffected"), IsExactPlaying(Fixture.Guard.Get(), AId, Fixture.MontageA.Get()));
				}
				bPassed &= CheckStage(*this, TEXT("Foreign.B.DestroyedRestoresA"), Fixture, Fixture.Coordinator.Get(),
					true, EStage::Executing, false, &A);
			});
			const float Duration = Fixture.Guard->Montage_Play(Fixture.MontageA.Get(), 1.0f,
				EMontagePlayReturnType::MontageLength, 0.0f, true);
			bPassed &= TestEqual(TEXT("Foreign.A.StartedExactlyOnce"), StartedA, 1);
			bPassed &= TestTrue(TEXT("Foreign.A.NativePositive"), FMath::IsFinite(Duration) && Duration > 0.0f);
			const FResult ResultA = A.Complete(Duration);
			bPassed &= TestTrue(TEXT("Foreign.A.Accepted"), ResultA.Outcome == EOutcome::Accepted);
			bPassed &= CheckStage(*this, TEXT("Foreign.A.Completed"), Fixture, Fixture.Coordinator.Get(),
				true, EStage::Returned, true, &A);
		}
	}
	bPassed &= CheckStage(*this, TEXT("Foreign.AllDestroyed"), Fixture, Fixture.Coordinator.Get(),
		false, EStage::NotEntered, false);
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageGuardStageLifecycleTest,
	"GGYGO.Animation.MontageGuard.StageQuery.Lifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMontageGuardStageLifecycleTest::RunTest(const FString& Parameters)
{
	FStageTestWorld World;
	FStageTestFixture Fixture;
	if (!Fixture.Initialize(*this, World.World)) { return false; }
	bool bPassed = true;
	int32 StartedCount = 0;
	FResult OldResult;
	{
		FScope Old(Fixture.MakeRequest(Fixture.MontageA.Get(), Fixture.Coordinator.Get()));
		const bool bCanExecute = Old.CanExecute();
		bPassed &= TestTrue(TEXT("Lifecycle.Old.NativeEntryAuthorized"), bCanExecute);
		if (!bCanExecute) { Old.Complete(0.0f); return false; }
		float Duration = 0.0f;
		{
			FStartedObservation Observation(Fixture.ObserverA.Get(), Fixture.Guard.Get(), Fixture.MontageA.Get(), [&]()
			{
				++StartedCount;
				bPassed &= TestTrue(TEXT("Lifecycle.Old.Started.RealPlayingInstance"),
					ReadActiveId(Fixture.Guard.Get(), Fixture.MontageA.Get()) != INDEX_NONE);
				bPassed &= CheckStage(*this, TEXT("Lifecycle.Old.Started"), Fixture, Fixture.Coordinator.Get(),
					true, EStage::Executing, false, &Old);
			});
			Duration = Fixture.Guard->Montage_Play(Fixture.MontageA.Get(), 1.0f,
				EMontagePlayReturnType::MontageLength, 0.0f, true);
		}
		bPassed &= TestEqual(TEXT("Lifecycle.Old.StartedExactlyOnce"), StartedCount, 1);
		bPassed &= TestTrue(TEXT("Lifecycle.Old.NativePositive"), FMath::IsFinite(Duration) && Duration > 0.0f);
		bPassed &= CheckStage(*this, TEXT("Lifecycle.Old.Returned"), Fixture, Fixture.Coordinator.Get(),
			true, EStage::Returned, false, &Old);
		// Full public engine lifecycle, safely after the native frame and observer have exited.
		Fixture.Guard->UninitializeAnimation();
		bPassed &= CheckStage(*this, TEXT("Lifecycle.Old.Uninitialized"), Fixture, Fixture.Coordinator.Get(),
			false, EStage::NotEntered, false, &Old);
		Fixture.Guard->InitializeAnimation();
		bPassed &= TestTrue(TEXT("Lifecycle.ReinitializedSameOriginalInstance"),
			Fixture.IsCoordinatorCurrent(Fixture.Coordinator.Get()));
		bPassed &= CheckStage(*this, TEXT("Lifecycle.Old.CannotReviveAfterInitialize"), Fixture,
			Fixture.Coordinator.Get(), false, EStage::NotEntered, false, &Old);
		OldResult = Old.Complete(Duration);
		bPassed &= TestTrue(TEXT("Lifecycle.Old.CompletedInvalid"), OldResult.Outcome == EOutcome::LifecycleInvalid);
		bPassed &= CheckStage(*this, TEXT("Lifecycle.Old.SealedStillUnavailable"), Fixture,
			Fixture.Coordinator.Get(), false, EStage::NotEntered, false, &Old);
	}
	bPassed &= CheckStage(*this, TEXT("Lifecycle.Old.Destroyed"), Fixture, Fixture.Coordinator.Get(),
		false, EStage::NotEntered, false);
	FResult NewResult;
	bPassed &= RunSingleCall(*this, Fixture, TEXT("Lifecycle.New"), NewResult);
	bPassed &= TestTrue(TEXT("Lifecycle.New.ActualIssuedGeneration"), OldResult.Identity.LifecycleGeneration != 0
		&& NewResult.Identity.LifecycleGeneration != 0
		&& NewResult.Identity.LifecycleGeneration != OldResult.Identity.LifecycleGeneration);
	bPassed &= TestTrue(TEXT("Lifecycle.New.CallIdNotReused"), OldResult.Identity.CallId != 0
		&& NewResult.Identity.CallId > OldResult.Identity.CallId);
	return bPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS

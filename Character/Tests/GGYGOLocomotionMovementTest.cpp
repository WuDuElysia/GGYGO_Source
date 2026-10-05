#include "Character/Tests/GGYGOLocomotionMovementTestTypes.h"
#include "Character/Components/GGYGOCurveRootMotionSource.h"

#include "Character/Data/GGYGOLocomotionMotionProfile.h"
#include "Character/Data/GGYGOMovementSet.h"
#include "Character/Data/GGYGOLocomotionEvaluation.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Input/GGYGOMovementInputTypes.h"
#include "InputAction.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#endif

AGGYGOLocomotionTestCharacter::AGGYGOLocomotionTestCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UGGYGOLocomotionTestMovementComponent>(
		ACharacter::CharacterMovementComponentName))
{
}

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	// Synthetic upstream protocol values exercise only the public CMC consumer.
	// They do not certify physical input or PlayerInput request issuance.
	class FLocomotionMovementConsumerFixture
	{
	public:
		FLocomotionMovementConsumerFixture(FAutomationTestBase& InTest,
			UGGYGOCharacterMovementComponent* InMovement, const TCHAR* InName)
			: Test(InTest), Movement(InMovement), Name(InName), Source(NewObject<UInputAction>(InMovement, NAME_None, RF_Transient))
		{
			Session.Producer = Source.Get();
			Session.SessionSerial = 1;
		}

		~FLocomotionMovementConsumerFixture() { Close(); }
		FLocomotionMovementConsumerFixture(const FLocomotionMovementConsumerFixture&) = delete;
		FLocomotionMovementConsumerFixture& operator=(const FLocomotionMovementConsumerFixture&) = delete;

		bool Bind()
		{
			UGGYGOCharacterMovementComponent* Consumer = Movement.Get();
			if (!Test.TestNotNull(Name + TEXT(" protocol consumer is live"), Consumer)) return false;
			if (!Test.TestNotNull(Name + TEXT(" synthetic protocol source is live"), Source.Get())) return false;
			FString Error;
			bBound = Consumer->BindMovementInputSession(Session,
				Consumer->GetMovementInputBindingSerial(), Binding, Error);
			if (!Check(TEXT("Bind"), bBound && Error.IsEmpty(), Error)) return false;
			if (!Check(TEXT("Bind retains original identities"), Binding.Consumer.Get() == Consumer
				&& Binding.ConsumerBindingSerial != 0 && Binding.SourceSession == Session, FString())) return false;
			return Emit(EGGYGOMovementInputFactKind::SessionOpened, 0, TEXT("Opened"));
		}

		bool StartFreshRequest(const TCHAR* Stage)
		{
			if (bRequestOpen && !Release(Stage)) return false;
			if (!Emit(EGGYGOMovementInputFactKind::NeutralConfirmed, 0, Stage)) return false;
			++SourceRequestSerial;
			if (!Emit(EGGYGOMovementInputFactKind::RequestStarted, SourceRequestSerial, Stage)) return false;
			bRequestOpen = true;
			return true;
		}

		bool Release(const TCHAR* Stage)
		{
			if (!Check(Stage, bRequestOpen, TEXT("exact source request must be open before Released"))) return false;
			if (!Emit(EGGYGOMovementInputFactKind::RequestReleased, SourceRequestSerial, Stage)) return false;
			bRequestOpen = false;
			return true;
		}

		bool Close()
		{
			bool bSucceeded = true;
			if (bBound)
			{
				// Teardown retires the original binding; it does not fabricate a release.
				bBound = false;
				UGGYGOCharacterMovementComponent* Consumer = Movement.Get();
				if (Test.TestNotNull(Name + TEXT(" teardown consumer is live"), Consumer))
				{
					FString Error;
					const bool bInvalidated = Consumer->InvalidateMovementInputSession(
						Binding, FName(TEXT("LocomotionConsumerFixtureEnded")), Error);
					bSucceeded = Check(TEXT("Invalidate original binding"), bInvalidated && Error.IsEmpty(), Error);
				}
				else
				{
					bSucceeded = false;
				}
			}
			bRequestOpen = false;
			Source.Reset();
			return bSucceeded;
		}

	private:
		bool Check(const TCHAR* Stage, bool bCondition, const FString& Error)
		{
			return Test.TestTrue(FString::Printf(TEXT("%s protocol %s (Error='%s')"),
				*Name, Stage, *Error), bCondition);
		}

		bool Emit(EGGYGOMovementInputFactKind Kind, uint64 RequestSerial, const TCHAR* Stage)
		{
			UGGYGOCharacterMovementComponent* Consumer = Movement.Get();
			if (!Test.TestNotNull(Name + TEXT(" fact consumer is live"), Consumer)) return false;
			if (!Check(Stage, bBound, TEXT("original consumer binding is required"))) return false;
			FGGYGOMovementInputFact Fact;
			Fact.Request.Session = Session;
			Fact.Request.RequestSerial = RequestSerial;
			Fact.EventSerial = ++EventSerial;
			Fact.Kind = Kind;
			Fact.SessionMode = Kind == EGGYGOMovementInputFactKind::SessionOpened
				? EGGYGOMovementInputSessionMode::Rearm : EGGYGOMovementInputSessionMode::Invalid;
			Fact.StartProof = Kind == EGGYGOMovementInputFactKind::RequestStarted
				? EGGYGOMovementInputStartProof::ReleasedThenPhysicalPress : EGGYGOMovementInputStartProof::Invalid;
			FString Error;
			const EGGYGOMovementInputConsumeResult Result = Consumer->ConsumeMovementInputFact(Binding, Fact, Error);
			return Check(*FString::Printf(TEXT("%s Kind=%u Request=%llu Event=%llu Result=%u"), Stage,
				static_cast<uint32>(Kind), static_cast<unsigned long long>(RequestSerial),
				static_cast<unsigned long long>(Fact.EventSerial), static_cast<uint32>(Result)),
				Result == EGGYGOMovementInputConsumeResult::Recorded && Error.IsEmpty(), Error);
		}

		FAutomationTestBase& Test;
		TWeakObjectPtr<UGGYGOCharacterMovementComponent> Movement;
		FString Name;
		TStrongObjectPtr<UObject> Source;
		FGGYGOMovementInputSessionIdentity Session;
		FGGYGOMovementInputConsumerBindingId Binding;
		uint64 SourceRequestSerial = 0;
		uint64 EventSerial = 0;
		bool bBound = false;
		bool bRequestOpen = false;
	};

	void AddLinearKeys(FRuntimeFloatCurve& Curve, float StartValue, float EndValue)
	{
		FRichCurve* RichCurve = Curve.GetRichCurve();
		RichCurve->SetKeyInterpMode(RichCurve->AddKey(0.0f, StartValue), RCIM_Linear);
		RichCurve->SetKeyInterpMode(RichCurve->AddKey(1.0f, EndValue), RCIM_Linear);
	}

	UGGYGOLocomotionMotionProfile* MakeProfile(UObject* Outer, bool bLoop, float StartSpeed, float EndSpeed, float EndYaw = 0.0f)
	{
		UGGYGOLocomotionMotionProfile* Profile = NewObject<UGGYGOLocomotionMotionProfile>(Outer);
		Profile->Duration = 1.0f;
		Profile->bLoop = bLoop;
		AddLinearKeys(Profile->SpeedCurve, StartSpeed, EndSpeed);
		AddLinearKeys(Profile->DirectionXCurve, 1.0f, 1.0f);
		AddLinearKeys(Profile->DirectionYCurve, 0.0f, 0.0f);
		AddLinearKeys(Profile->YawCurve, 0.0f, EndYaw);
		return Profile;
	}

	bool BindMovementFixture(UGGYGOCharacterMovementComponent* Move, const UGGYGOMovementSet* Set, FString* OutError = nullptr)
	{
		if (!Move->SetMovementSet(Set, OutError)) return false;
		if (!Set->bUseCurveDrivenSpeed) return true;
		ACharacter* Character = Cast<ACharacter>(Move->GetOwner());
		if (!Character) return false;
		USkeletalMeshComponent* Mesh = Character->GetMesh();
		UGGYGOLocomotionTestAnimInstance* Producer = Cast<UGGYGOLocomotionTestAnimInstance>(Mesh->GetAnimInstance());
		if (!Producer)
		{
			// Synthetic primary producer on a skeleton-free test character; still enters the real publication identity gate.
			Producer = NewObject<UGGYGOLocomotionTestAnimInstance>(Mesh);
			Mesh->AnimScriptInstance = Producer;
		}
		FGGYGOLocomotionSourceBinding Binding;
		Binding.Identity.Producer = Producer; Binding.Identity.Character = Character; Binding.Identity.Mesh = Mesh;
		Binding.Identity.LifecycleGeneration = 1; Binding.Identity.ConfigurationGeneration = ++Producer->PublicationGeneration;
		Binding.Status = EGGYGOLocomotionSourceStatus::Available;
		const auto Source = [](const UGGYGOLocomotionMotionProfile* Profile, EGGYGOLocomotionMotionType Type, FName Key)
		{
			FGGYGOLocomotionSequenceSource Result;
			Result.MotionType = Type; Result.RouteKey = Key;
			if (Profile)
			{
				// Preserve the original strict fixture values/tangents; only adapt their public evaluation boundary.
				UGGYGOLocomotionTestSequence* Sequence = NewObject<UGGYGOLocomotionTestSequence>(const_cast<UGGYGOLocomotionMotionProfile*>(Profile));
				Sequence->TestLength = Profile->Duration; Sequence->RateScale = 1.0f;
				Sequence->TestCurves.Add(TEXT("RootMotion_Speed"), *Profile->SpeedCurve.GetRichCurveConst());
				Sequence->TestCurves.Add(TEXT("RootMotion_DirX"), *Profile->DirectionXCurve.GetRichCurveConst());
				Sequence->TestCurves.Add(TEXT("RootMotion_DirY"), *Profile->DirectionYCurve.GetRichCurveConst());
				Sequence->TestCurves.Add(TEXT("RootMotion_Yaw"), *Profile->YawCurve.GetRichCurveConst());
				Result.Sequence.Reset(Sequence); Result.PlayLength = Profile->Duration;
				Result.SequenceRateScale = Sequence->RateScale; Result.bLoop = Profile->bLoop;
			}
			return Result;
		};
		Binding.SingleSources = {
			Source(Set->WalkStartProfile, EGGYGOLocomotionMotionType::WalkStart, TEXT("WalkStart")),
			Source(Set->StartStopProfile, EGGYGOLocomotionMotionType::StartStop, TEXT("WalkStartEnd")),
			Source(Set->WalkStopProfile, EGGYGOLocomotionMotionType::WalkStop, TEXT("WalkEnd")),
			Source(Set->RunStopProfile, EGGYGOLocomotionMotionType::RunStop, TEXT("RunEnd")),
			Source(Set->TurnBackProfile, EGGYGOLocomotionMotionType::TurnBack, TEXT("TurnBack"))};
		FGGYGOLocomotionBlendSpaceSampleSource Walk, Run;
		Walk.Source = Source(Set->WalkLoopProfile, EGGYGOLocomotionMotionType::WalkRun, TEXT("walkRun"));
		Run.Source = Source(Set->RunLoopProfile, EGGYGOLocomotionMotionType::WalkRun, TEXT("walkRun"));
		Walk.SampleIndex = 0; Walk.SampleRateScale = 1.0f;
		Run.SampleIndex = 1; Run.SampleRateScale = 1.0f; Run.SampleValue = FVector(1.0, 0.0, 0.0);
		FString FixtureError;
		UBlendSpace1D* BS = MakeGGYGOLocomotionTestBlendSpace(Character, Walk.Source.Sequence.Get(), Run.Source.Sequence.Get(), FixtureError);
		if (!BS)
		{
			if (OutError) *OutError = FixtureError;
			return false;
		}
		Binding.WalkRunKey = TEXT("walkRun"); Binding.WalkRunBlendSpace.Reset(BS); Binding.bWalkRunLoop = true;
		Binding.WalkRunSamples = {Walk, Run};
		FString Error;
		const bool bPublished = Move->PublishLocomotionSourceBinding(Binding, Error);
		if (OutError) *OutError = Error;
		return bPublished;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOLocomotionMovementTest,
	"GGYGO.Movement.Locomotion.AuthorityAndMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOLocomotionMovementTest::RunTest(const FString& Parameters)
{
	UWorld::InitializationValues Init;
	Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
		.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("World"), World)) return false;
	struct FCleanup { UWorld* World; ~FCleanup() { World->DestroyWorld(false); } } Cleanup{World};

	AGGYGOLocomotionTestCharacter* Character = World->SpawnActor<AGGYGOLocomotionTestCharacter>();
	if (!TestNotNull(TEXT("Character"), Character)) return false;
	UGGYGOLocomotionTestMovementComponent* Move =
		Cast<UGGYGOLocomotionTestMovementComponent>(Character->GetCharacterMovement());
	if (!TestNotNull(TEXT("Custom movement"), Move)) return false;
	Move->SetUpdatedComponent(Character->GetCapsuleComponent());
	Move->MovementMode = MOVE_Walking;

	UGGYGOMovementSet* FirstSet = NewObject<UGGYGOMovementSet>(Character);
	// Blend and stop selection use the curve-backed configuration.
	FirstSet->bUseCurveDrivenSpeed = true;
	FirstSet->MaxAcceleration = 1234.0f;
	FirstSet->GroundFriction = 3.0f;
	FirstSet->WalkRunBlendInterpSpeed = 6.0f;
	FirstSet->WalkStartProfile = MakeProfile(FirstSet, false, 0.0f, 200.0f);
	FirstSet->WalkLoopProfile = MakeProfile(FirstSet, true, 200.0f, 220.0f);
	FirstSet->RunLoopProfile = MakeProfile(FirstSet, true, 400.0f, 500.0f);
	FirstSet->StartStopProfile = MakeProfile(FirstSet, false, 200.0f, 0.0f);
	FirstSet->WalkStopProfile = MakeProfile(FirstSet, false, 220.0f, 0.0f);
	FirstSet->RunStopProfile = MakeProfile(FirstSet, false, 500.0f, 0.0f);
	FirstSet->TurnBackProfile = MakeProfile(FirstSet, false, 500.0f, 0.0f, 180.0f);

	const float DefaultAcceleration = Move->MaxAcceleration;
	const float DefaultFriction = Move->GroundFriction;
	if (!TestTrue(TEXT("Curve MovementSet binding succeeds"), BindMovementFixture(Move, FirstSet))) return false;
	TestEqual(TEXT("MovementSet applies acceleration"), Move->MaxAcceleration, 1234.0f);
	TestEqual(TEXT("MovementSet applies friction"), Move->GroundFriction, 3.0f);
	if (!TestFalse(TEXT("Legal null detach leaves no admitted MovementSet"), Move->SetMovementSet(nullptr))) return false;
	TestEqual(TEXT("Null MovementSet restores component acceleration"), Move->MaxAcceleration, DefaultAcceleration);
	TestEqual(TEXT("Null MovementSet restores component friction"), Move->GroundFriction, DefaultFriction);
	if (!TestTrue(TEXT("Curve MovementSet rebinding succeeds"), BindMovementFixture(Move, FirstSet))) return false;
	FLocomotionMovementConsumerFixture MovementSource(*this, Move, TEXT("Authority/mapping"));
	if (!MovementSource.Bind() || !MovementSource.StartFreshRequest(TEXT("Initial curve scene"))) return false;

	Move->SetTestAcceleration(FVector::ForwardVector);
	Move->SetTestGait(EGGYGOGait::Run);
	Move->AdvanceTestBlend(0.1f);
	TestTrue(TEXT("WalkRun alpha advances in Movement"), FMath::IsNearlyEqual(Move->GetWalkRunBlendAlpha(), 0.6f));

	if (!MovementSource.Release(TEXT("WalkStart stop scene"))) return false;
	Move->SetTestAcceleration(FVector::ZeroVector);
	Move->SetTestMotion(EGGYGOLocomotionMotionType::WalkStart, 0.2f);
	Move->AdvanceTestMotion(0.016f, true, EGGYGOGait::Walk);
	TestEqual(TEXT("Stopping during WalkStart selects StartStop"), Move->GetStopMotionType(), EGGYGOStopMotionType::StartStop);

	if (!MovementSource.StartFreshRequest(TEXT("RunStop scene"))
		|| !MovementSource.Release(TEXT("RunStop scene"))) return false;
	Move->SetTestGait(EGGYGOGait::Run);
	Move->SetTestMotion(EGGYGOLocomotionMotionType::WalkRun, 0.0f);
	FNetworkPredictionData_Client_Character* ClientData = Move->GetPredictionData_Client_Character();
	FSavedMovePtr RecordedPtr(new FSavedMove_GGYGO());
	FSavedMove_GGYGO* Recorded = static_cast<FSavedMove_GGYGO*>(RecordedPtr.Get());
	Recorded->SetMoveFor(Character, 0.016f, FVector::ZeroVector, *ClientData);
	Move->AdvanceTestMotion(0.016f, true, EGGYGOGait::Run);
	Recorded->PostUpdate(Character, FSavedMove_Character::PostUpdate_Record);
	TestEqual(TEXT("Stopping from Run selects RunStop"), Move->GetStopMotionType(), EGGYGOStopMotionType::RunStop);

	if (!TestTrue(TEXT("RunStop produces a usable finite curve source sample"),
		Move->GetTestCurveMotion().HasUsableSpeed()
			&& FMath::IsFinite(Move->GetTestCurveMotion().Speed))) return false;
	const FGGYGOLocomotionCurveSample RunStopCurveMotion = Move->GetTestCurveMotion();
	if (!TestTrue(TEXT("Original Stop SavedMove captures the actual native prepared interval"), Recorded->SavedCurveRootMotionPrepared.IsValid())) return false;
	const auto OriginalPrepared = Recorded->SavedCurveRootMotionPrepared;
	// This local resource fixture is an unpossessed authority, not an autonomous player's prediction loop.
	// Preserve the actual original native group at the record boundary; PerformMovement does this automatically
	// for locally controlled autonomous pawns. No interval, prepared output or replay result is authored here.
	Recorded->SavedRootMotion = Move->CurrentRootMotion;
	const auto ContainsOriginalPreparedSource = [&OriginalPrepared](const TArray<TSharedPtr<FRootMotionSource>>& Sources)
	{
		return Sources.ContainsByPredicate([&OriginalPrepared](const TSharedPtr<FRootMotionSource>& Source)
		{
			if (!Source.IsValid() || Source->GetScriptStruct() != FRootMotionSource_GGYGOCurve::StaticStruct()) return false;
			const FRootMotionSource_GGYGOCurve* Curve = static_cast<const FRootMotionSource_GGYGOCurve*>(Source.Get());
			return Curve->Origin.IsValid() && Curve->Prepared == OriginalPrepared
				&& Curve->GetTime() == OriginalPrepared->NativeEndTime;
		});
	};
	if (!TestTrue(TEXT("Original resource snapshot retains the actual native source/Prepared and endpoint"),
		ContainsOriginalPreparedSource(Recorded->SavedRootMotion.RootMotionSources)
			|| ContainsOriginalPreparedSource(Recorded->SavedRootMotion.PendingAddRootMotionSources))) return false;
	if (!TestTrue(TEXT("Original Stop move retains a valid released source checkpoint"),
		Recorded->SavedMovementInputSourceCheckpoint.bPresent
			&& !Recorded->SavedMovementInputSourceCheckpoint.bConsumerInvalidated
			&& Recorded->SavedMovementInputSourceCheckpoint.RequestSerial != 0)) return false;
	Move->SetTestMotion(EGGYGOLocomotionMotionType::RunStop, 0.4f, EGGYGOStopMotionType::RunStop);
	ClientData->SavedMoves.Add(RecordedPtr);
	Character->bClientUpdating = true;
	Move->SetAuthorityReplayForTest(true);
	Recorded->PrepMoveFor(Character);
	Move->UpdateCharacterStateBeforeMovement(0.016f);
	Character->bClientUpdating = false;
	Move->SetAuthorityReplayForTest(false);
	ClientData->SavedMoves.RemoveSingle(RecordedPtr);
	const bool bAuthorityTime = TestEqual(TEXT("Stop correction recomputes motion time from authority baseline"), Move->GetTestMotionTime(), 0.416f, 0.000001f);
	const bool bAuthoritySpeed = TestEqual(TEXT("Stop correction recomputes source speed instead of restoring old Prepared"), Move->GetCurveMotion().Speed, 292.0f, 0.0001f);
	const bool bOriginalPreparedUnchanged = TestTrue(TEXT("Original prepared interval remains immutable"),
		Recorded->SavedCurveRootMotionPrepared == OriginalPrepared && OriginalPrepared->MotionEndTime == 0.016f);
	if (!bAuthorityTime || !bAuthoritySpeed || !bOriginalPreparedUnchanged) return false;

	// Fixed mode reuses all seven valid profiles and is configured before binding.
	UGGYGOMovementSet* FixedSet = NewObject<UGGYGOMovementSet>(Character);
	FixedSet->bUseCurveDrivenSpeed = false;
	FixedSet->WalkStartProfile = FirstSet->WalkStartProfile;
	FixedSet->WalkLoopProfile = FirstSet->WalkLoopProfile;
	FixedSet->RunLoopProfile = FirstSet->RunLoopProfile;
	FixedSet->StartStopProfile = FirstSet->StartStopProfile;
	FixedSet->WalkStopProfile = FirstSet->WalkStopProfile;
	FixedSet->RunStopProfile = FirstSet->RunStopProfile;
	FixedSet->TurnBackProfile = FirstSet->TurnBackProfile;
	if (!TestTrue(TEXT("Explicit fixed MovementSet binding succeeds"), BindMovementFixture(Move, FixedSet))) return false;
	if (!MovementSource.StartFreshRequest(TEXT("Explicit fixed rebind scene"))) return false;

	// Restore the real old sample after binding cleanup to test this update's cleanup.
	Move->SetTestCurveMotion(RunStopCurveMotion);
	Move->SetTestTurnBackPhase(EGGYGOTurnBackPhase::Turning);
	Move->SetTestAcceleration(-FVector::ForwardVector);
	Move->SetTestGait(EGGYGOGait::Run);
	if (!TestTrue(TEXT("Curve-off precondition retains the usable finite RunStop sample"),
		Move->GetTestCurveMotion().HasUsableSpeed()
			&& FMath::IsFinite(Move->GetTestCurveMotion().Speed))) return false;
	if (!TestEqual(TEXT("Curve-off precondition retains Turning after binding"),
		Move->GetTurnBackPhase(), EGGYGOTurnBackPhase::Turning)) return false;
	Move->AdvanceTestMotion(0.016f, true, EGGYGOGait::Run);
	TestEqual(TEXT("Curve-off clears TurnBack phase"), Move->GetTurnBackPhase(), EGGYGOTurnBackPhase::None);
	TestFalse(TEXT("Curve-off produces no curve source sample"), Move->GetTestCurveMotion().bHasCurveSource);
	if (!TestTrue(TEXT("Curve mode is rebound before later state setup"), BindMovementFixture(Move, FirstSet))) return false;
	if (!MovementSource.StartFreshRequest(TEXT("Curve rebind authority scene"))) return false;

	Character->SetActorRotation(FRotator::ZeroRotator);
	Move->Velocity = FVector(100.0f, 50.0f, 0.0f);
	float Forward = 0.0f;
	float Right = 0.0f;
	Move->GetLocalVelocityAxes(Forward, Right);
	TestTrue(TEXT("CMC local X is forward"), FMath::IsNearlyEqual(Forward, 100.0f / FMath::Sqrt(12500.0f)));
	TestTrue(TEXT("CMC local Y is right"), FMath::IsNearlyEqual(Right, 50.0f / FMath::Sqrt(12500.0f)));

	FSavedMove_GGYGO Earlier;
	Earlier.SavedLocomotionMotionType = EGGYGOLocomotionMotionType::WalkRun;
	FSavedMovePtr LaterPtr(new FSavedMove_GGYGO());
	static_cast<FSavedMove_GGYGO*>(LaterPtr.Get())->SavedLocomotionMotionType = EGGYGOLocomotionMotionType::RunStop;
	TestFalse(TEXT("Different motion segments cannot combine"), Earlier.CanCombineWith(
		LaterPtr, Character, 0.125f));

	Move->SetTestMotion(EGGYGOLocomotionMotionType::RunStop, 0.75f, EGGYGOStopMotionType::RunStop);
	Move->SetTestSequence(42);
	Move->SetAuthorityReplayForTest(true);
	FSavedMove_GGYGO StaleMove;
	StaleMove.SavedLocomotionMotionType = EGGYGOLocomotionMotionType::WalkStart;
	StaleMove.SavedLocomotionMotionTime = 0.1f;
	StaleMove.SavedLocomotionMotionSequence = 7;
	StaleMove.PrepMoveFor(Character);
	Move->SetAuthorityReplayForTest(false);
	TestEqual(TEXT("Correction replay keeps authoritative motion"), Move->GetLocomotionMotionType(), EGGYGOLocomotionMotionType::RunStop);
	TestEqual(TEXT("Correction replay keeps authoritative time"), Move->GetTestMotionTime(), 0.75f);
	TestEqual(TEXT("Correction replay keeps authoritative sequence"), Move->GetTestSequence(), static_cast<uint16>(42));

	UGGYGOLocomotionMotionProfile* LoopProfile = MakeProfile(Character, true, 0.0f, 100.0f, 180.0f);
	FGGYGOLocomotionCurveSample LoopSample;
	TestTrue(TEXT("Loop profile evaluates across boundary"), LoopProfile->EvaluateInterval(0.9f, 1.1f, LoopSample));
	TestTrue(TEXT("Loop yaw unwraps across boundary"), FMath::IsNearlyEqual(LoopSample.YawDeltaDegrees, 36.0f, 0.01f));
	TestTrue(TEXT("Loop endpoint speed uses wrapped phase"), FMath::IsNearlyEqual(LoopSample.Speed, 10.0f, 0.01f));

	// Isolate speed-consumer checks from the original authority and mapping fixture.
	AGGYGOLocomotionTestCharacter* SpeedCharacter = World->SpawnActor<AGGYGOLocomotionTestCharacter>();
	if (!TestNotNull(TEXT("Speed-consumer character"), SpeedCharacter)) return false;
	UGGYGOLocomotionTestMovementComponent* SpeedMove =
		Cast<UGGYGOLocomotionTestMovementComponent>(SpeedCharacter->GetCharacterMovement());
	if (!TestNotNull(TEXT("Speed-consumer movement"), SpeedMove)) return false;
	SpeedMove->SetUpdatedComponent(SpeedCharacter->GetCapsuleComponent());
	SpeedMove->MovementMode = MOVE_Walking;

	constexpr float TinySpeed = 1.0f / 32768.0f;
	UGGYGOMovementSet* SpeedSet = NewObject<UGGYGOMovementSet>(SpeedCharacter);
	SpeedSet->bUseCurveDrivenSpeed = true;
	SpeedSet->WalkSpeed = 64.0f;
	SpeedSet->RunSpeed = 512.0f;
	SpeedSet->RootMotionScale = 2.0f;
	SpeedSet->WalkStartProfile = MakeProfile(SpeedSet, false, 0.0f, 0.0f);
	SpeedSet->WalkLoopProfile = MakeProfile(SpeedSet, true, TinySpeed, TinySpeed);
	SpeedSet->RunLoopProfile = MakeProfile(SpeedSet, true, 128.0f, 128.0f);
	SpeedSet->StartStopProfile = FirstSet->StartStopProfile;
	SpeedSet->WalkStopProfile = FirstSet->WalkStopProfile;
	SpeedSet->RunStopProfile = FirstSet->RunStopProfile;
	SpeedSet->TurnBackProfile = FirstSet->TurnBackProfile;
	const auto MakeScaledSpeedSet = [SpeedCharacter, SpeedSet](bool bCurveMode, float Scale)
	{
		UGGYGOMovementSet* Set = NewObject<UGGYGOMovementSet>(SpeedCharacter);
		Set->bUseCurveDrivenSpeed = bCurveMode;
		Set->WalkSpeed = 64.0f;
		Set->RunSpeed = 512.0f;
		Set->RootMotionScale = Scale;
		Set->WalkStartProfile = SpeedSet->WalkStartProfile;
		Set->WalkLoopProfile = SpeedSet->WalkLoopProfile;
		Set->RunLoopProfile = SpeedSet->RunLoopProfile;
		Set->StartStopProfile = SpeedSet->StartStopProfile;
		Set->WalkStopProfile = SpeedSet->WalkStopProfile;
		Set->RunStopProfile = SpeedSet->RunStopProfile;
		Set->TurnBackProfile = SpeedSet->TurnBackProfile;
		return Set;
	};
	UGGYGOMovementSet* ZeroScaleSet = MakeScaledSpeedSet(true, 0.0f);
	UGGYGOMovementSet* FixedSpeedSet = MakeScaledSpeedSet(false, 2.0f);
	UGGYGOMovementSet* OverflowScaleSet = MakeScaledSpeedSet(true, MAX_flt);
	if (!TestTrue(TEXT("Speed-consumer curve configuration binds"), BindMovementFixture(SpeedMove, SpeedSet))) return false;
	FLocomotionMovementConsumerFixture SpeedSource(*this, SpeedMove, TEXT("Speed consumer"));
	if (!SpeedSource.Bind() || !SpeedSource.StartFreshRequest(TEXT("Initial speed scene"))) return false;

	FString SampleError;
	FGGYGOLocomotionCurveSample ZeroSpeedSample;
	if (!TestTrue(TEXT("Zero-speed Profile genuinely evaluates"),
		SpeedSet->WalkStartProfile->EvaluateInterval(0.25f, 0.5f, ZeroSpeedSample, &SampleError))) return false;
	if (!TestTrue(TEXT("Zero-speed sample retains successful source and exact zero"),
		ZeroSpeedSample.bHasCurveSource && ZeroSpeedSample.Speed == 0.0f)) return false;
	TestFalse(TEXT("Zero speed keeps the shared positive-speed predicate false"), ZeroSpeedSample.HasUsableSpeed());
	SpeedMove->SetTestCurveMotion(ZeroSpeedSample);
	SpeedMove->SetTestGait(EGGYGOGait::Run);
	TestTrue(TEXT("Zero-speed source qualifies for curve consumption"), SpeedMove->IsCurveDrivingSpeed());
	TestTrue(TEXT("Zero-speed consumer preserves exact zero instead of fixed Run"), SpeedMove->GetMaxSpeed() == 0.0f);

	FGGYGOLocomotionCurveSample TinySpeedSample;
	if (!TestTrue(TEXT("Tiny-speed Profile genuinely evaluates"),
		SpeedSet->WalkLoopProfile->EvaluateInterval(0.25f, 0.5f, TinySpeedSample, &SampleError))) return false;
	if (!TestTrue(TEXT("Tiny-speed sample retains successful source and exact authored speed"),
		TinySpeedSample.bHasCurveSource && TinySpeedSample.Speed == TinySpeed)) return false;
	TestFalse(TEXT("Tiny speed keeps the shared positive-speed predicate false"), TinySpeedSample.HasUsableSpeed());
	SpeedMove->SetTestCurveMotion(TinySpeedSample);
	SpeedMove->SetTestGait(EGGYGOGait::Run);
	TestTrue(TEXT("Tiny-speed source qualifies for curve consumption"), SpeedMove->IsCurveDrivingSpeed());
	TestTrue(TEXT("Tiny-speed consumer preserves the exact scaled value"), SpeedMove->GetMaxSpeed() == 1.0f / 16384.0f);

	FGGYGOLocomotionCurveSample PositiveSpeedSample;
	if (!TestTrue(TEXT("Positive-speed Profile genuinely evaluates"),
		SpeedSet->RunLoopProfile->EvaluateInterval(0.25f, 0.5f, PositiveSpeedSample, &SampleError))) return false;
	if (!TestTrue(TEXT("Positive-speed sample retains successful source and exact authored speed"),
		PositiveSpeedSample.bHasCurveSource && PositiveSpeedSample.Speed == 128.0f)) return false;
	SpeedMove->SetTestCurveMotion(PositiveSpeedSample);
	SpeedMove->SetTestGait(EGGYGOGait::Run);
	TestTrue(TEXT("Positive-speed source qualifies for curve consumption"), SpeedMove->IsCurveDrivingSpeed());
	TestTrue(TEXT("Positive-speed consumer returns the exact scaled value"), SpeedMove->GetMaxSpeed() == 256.0f);

	SpeedMove->SetForceWalkRequested(true);
	TestTrue(TEXT("ForceWalk limits the curve upper bound exactly"), SpeedMove->GetMaxSpeed() == 64.0f);
	SpeedMove->SetTestCurveMotion(ZeroSpeedSample);
	SpeedMove->SetTestGait(EGGYGOGait::Run);
	TestTrue(TEXT("ForceWalk retains the zero-speed curve source"), SpeedMove->IsCurveDrivingSpeed());
	TestTrue(TEXT("ForceWalk preserves exact zero instead of fixed Walk"), SpeedMove->GetMaxSpeed() == 0.0f);
	SpeedMove->SetForceWalkRequested(false);
	TestFalse(TEXT("Speed fixture clears its ForceWalk request"), SpeedMove->IsForceWalkRequested());

	if (!TestTrue(TEXT("Explicit zero-Scale configuration binds"), BindMovementFixture(SpeedMove, ZeroScaleSet))) return false;
	if (!SpeedSource.StartFreshRequest(TEXT("Zero-Scale rebind scene"))) return false;
	SpeedMove->SetTestCurveMotion(PositiveSpeedSample);
	SpeedMove->SetTestGait(EGGYGOGait::Run);
	TestTrue(TEXT("Zero Scale retains a successful curve source"), SpeedMove->IsCurveDrivingSpeed());
	TestTrue(TEXT("Zero-Scale consumer preserves exact zero"), SpeedMove->GetMaxSpeed() == 0.0f);

	if (!TestTrue(TEXT("Explicit fixed speed configuration binds"), BindMovementFixture(SpeedMove, FixedSpeedSet))) return false;
	if (!SpeedSource.StartFreshRequest(TEXT("Fixed speed rebind scene"))) return false;
	UGGYGOLocomotionTestAnimInstance* FixedProducer = CastChecked<UGGYGOLocomotionTestAnimInstance>(SpeedCharacter->GetMesh()->GetAnimInstance());
	FGGYGOLocomotionSourceBinding OptionalMissing;
	OptionalMissing.Identity.Producer = FixedProducer; OptionalMissing.Identity.Character = SpeedCharacter;
	OptionalMissing.Identity.Mesh = SpeedCharacter->GetMesh(); OptionalMissing.Identity.LifecycleGeneration = 1;
	OptionalMissing.Identity.ConfigurationGeneration = ++FixedProducer->PublicationGeneration;
	OptionalMissing.Status = EGGYGOLocomotionSourceStatus::Missing;
	OptionalMissing.Error = TEXT("Fixed character does not provide the optional locomotion source resolver");
	FString PublicationError;
	if (!TestTrue(TEXT("Fixed mode receives Missing capability without rejecting the normal character"),
		SpeedMove->PublishLocomotionSourceBinding(OptionalMissing, PublicationError) && PublicationError.IsEmpty())) return false;
	if (!TestFalse(TEXT("A duplicate old producer generation cannot be republished"),
		SpeedMove->PublishLocomotionSourceBinding(OptionalMissing, PublicationError))
		|| !TestTrue(TEXT("Duplicate publication provides a generation diagnosis"), PublicationError.Contains(TEXT("generation")))) return false;
	SpeedMove->SetTestCurveMotion(PositiveSpeedSample);
	SpeedMove->SetTestGait(EGGYGOGait::Walk);
	TestFalse(TEXT("Explicit fixed mode does not qualify a curve source"), SpeedMove->IsCurveDrivingSpeed());
	TestTrue(TEXT("Explicit fixed Walk retains configured speed exactly"), SpeedMove->GetMaxSpeed() == 64.0f);
	SpeedMove->SetTestGait(EGGYGOGait::Run);
	TestTrue(TEXT("Explicit fixed Run retains configured speed exactly"), SpeedMove->GetMaxSpeed() == 512.0f);

	if (!TestTrue(TEXT("Curve configuration rebinds before failure qualification"), BindMovementFixture(SpeedMove, SpeedSet))) return false;
	if (!SpeedSource.StartFreshRequest(TEXT("Curve qualification rebind scene"))) return false;
	FGGYGOLocomotionCurveSample FailedSample = PositiveSpeedSample;
	if (!TestFalse(TEXT("Real reversed interval evaluation fails"),
		SpeedSet->RunLoopProfile->EvaluateInterval(0.5f, 0.25f, FailedSample, &SampleError))) return false;
	if (!TestTrue(TEXT("Failed evaluation reports its error and clears the source"),
		!SampleError.IsEmpty() && !FailedSample.bHasCurveSource)) return false;
	SpeedMove->SetTestCurveMotion(FailedSample);
	SpeedMove->SetTestGait(EGGYGOGait::Run);
	TestFalse(TEXT("Failed evaluation cannot qualify as successful zero speed"), SpeedMove->IsCurveDrivingSpeed());

	if (!TestTrue(TEXT("Finite MAX_flt Scale configuration genuinely binds"), BindMovementFixture(SpeedMove, OverflowScaleSet))) return false;
	if (!SpeedSource.StartFreshRequest(TEXT("Overflow qualification rebind scene"))) return false;
	SpeedMove->SetTestCurveMotion(PositiveSpeedSample);
	SpeedMove->SetTestGait(EGGYGOGait::Run);
	const float OverflowProduct = PositiveSpeedSample.Speed * OverflowScaleSet->RootMotionScale;
	if (!TestTrue(TEXT("Overflow fixture has finite operands and a non-finite product"),
		FMath::IsFinite(PositiveSpeedSample.Speed) && FMath::IsFinite(OverflowScaleSet->RootMotionScale)
			&& !FMath::IsFinite(OverflowProduct))) return false;
	TestFalse(TEXT("Overflowing scaled speed cannot qualify as a successful source"), SpeedMove->IsCurveDrivingSpeed());
	// Failure qualification does not assert the pending GetMaxSpeed failure-propagation contract.
	if (!SpeedSource.Close()) return false;
	if (!MovementSource.Close()) return false;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOLocomotionInputConsumerModeTest,
	"GGYGO.Movement.Locomotion.InputConsumer.ColdAndRearm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOLocomotionInputConsumerModeTest::RunTest(const FString& Parameters)
{
	UWorld::InitializationValues Init;
	Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
		.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("Consumer protocol world"), World)) return false;
	struct FProtocolWorldCleanup
	{
		UWorld* World;
		~FProtocolWorldCleanup() { World->DestroyWorld(false); }
	} WorldCleanup{World};

	// Only owns original public binding resources; no physical or execution state.
	struct FConsumerBindingCleanup
	{
		FAutomationTestBase& Test;
		TWeakObjectPtr<UGGYGOCharacterMovementComponent> Movement;
		FGGYGOMovementInputConsumerBindingId Binding;
		TStrongObjectPtr<UObject> SourceMarker;
		FString Name;

		~FConsumerBindingCleanup()
		{
			if (Binding.ConsumerBindingSerial != 0)
			{
				UGGYGOCharacterMovementComponent* Consumer = Movement.Get();
				if (Test.TestNotNull(Name + TEXT(" cleanup consumer"), Consumer))
				{
					FString Error;
					const bool bInvalidated = Consumer->InvalidateMovementInputSession(
						Binding, FName(TEXT("ColdRearmConsumerTestEnded")), Error);
					Test.TestTrue(FString::Printf(TEXT("%s invalidates original binding (Error='%s')"),
						*Name, *Error), bInvalidated && Error.IsEmpty());
				}
			}
			SourceMarker.Reset();
		}
	};

	using EKind = EGGYGOMovementInputFactKind;
	using EMode = EGGYGOMovementInputSessionMode;
	using EProof = EGGYGOMovementInputStartProof;
	using EResult = EGGYGOMovementInputConsumeResult;
	const auto MakeFact = [](const FGGYGOMovementInputSessionIdentity& Session, EKind Kind,
		uint64 RequestSerial, uint64 EventSerial, EMode Mode, EProof Proof, FName Reason)
	{
		FGGYGOMovementInputFact Fact;
		Fact.Request.Session = Session;
		Fact.Request.RequestSerial = RequestSerial;
		Fact.EventSerial = EventSerial;
		Fact.Kind = Kind;
		Fact.SessionMode = Mode;
		Fact.StartProof = Proof;
		Fact.Reason = Reason;
		return Fact;
	};
	const auto Consume = [this](UGGYGOCharacterMovementComponent* Move,
		const FGGYGOMovementInputConsumerBindingId& Binding, const FGGYGOMovementInputFact& Fact,
		EResult Expected, bool bEmptyError, const TCHAR* Stage)
	{
		FString Error;
		const EResult Actual = Move->ConsumeMovementInputFact(Binding, Fact, Error);
		if (!TestTrue(FString::Printf(TEXT("%s Result=%u Expected=%u (Error='%s')"), Stage,
			static_cast<uint32>(Actual), static_cast<uint32>(Expected), *Error), Actual == Expected)) return false;
		if (!TestTrue(FString::Printf(TEXT("%s preserves expected error presence (Error='%s')"), Stage, *Error),
			bEmptyError ? Error.IsEmpty() : !Error.IsEmpty())) return false;
		if (Fact.Kind == EKind::SourceUnresolved && Expected == EResult::Recorded)
		{
			return TestTrue(TEXT("Unresolved reports its explicit source reason"),
				Error.Contains(Fact.Reason.ToString()));
		}
		return true;
	};
	const auto Bind = [this](FConsumerBindingCleanup& Cleanup, FGGYGOMovementInputSessionIdentity& Session)
	{
		UGGYGOCharacterMovementComponent* Move = Cleanup.Movement.Get();
		if (!TestNotNull(Cleanup.Name + TEXT(" consumer"), Move)) return false;
		if (!TestNotNull(Cleanup.Name + TEXT(" concrete synthetic identity"), Cleanup.SourceMarker.Get())) return false;
		Session.Producer = Cleanup.SourceMarker.Get();
		Session.SessionSerial = 1;
		FString Error;
		if (!TestTrue(Cleanup.Name + TEXT(" public Bind"),
			Move->BindMovementInputSession(Session, Move->GetMovementInputBindingSerial(), Cleanup.Binding, Error))) return false;
		if (!TestTrue(Cleanup.Name + TEXT(" Bind clears error"), Error.IsEmpty())) return false;
		return TestTrue(Cleanup.Name + TEXT(" retains original binding identities"),
			Cleanup.Binding.Consumer.Get() == Move && Cleanup.Binding.ConsumerBindingSerial != 0
				&& Cleanup.Binding.SourceSession == Session);
	};
	const auto EvaluateHealthy = [this](UGGYGOLocomotionTestMovementComponent* Move, const TCHAR* Stage)
	{
		// Numeric intent only; the admitted request comes exclusively from public Consume.
		Move->SetTestAcceleration(FVector::ForwardVector);
		Move->UpdateCharacterStateBeforeMovement(0.016f);
		return TestTrue(Stage, Move->GetCurveMotion().bHasCurveSource
			&& Move->GetCurveMotion().Speed == 64.0f
			&& FMath::IsFinite(Move->GetCurveMotion().Speed)
			&& Move->GetResolvedGait() == EGGYGOGait::Walk
			&& Move->GetLocomotionMotionType() == EGGYGOLocomotionMotionType::WalkStart
			&& Move->GetTestMotionTime() > 0.0f);
	};

	AGGYGOLocomotionTestCharacter* Character = World->SpawnActor<AGGYGOLocomotionTestCharacter>();
	if (!TestNotNull(TEXT("Cold consumer character"), Character)) return false;
	UGGYGOLocomotionTestMovementComponent* Move =
		Cast<UGGYGOLocomotionTestMovementComponent>(Character->GetCharacterMovement());
	if (!TestNotNull(TEXT("Cold consumer actual CMC"), Move)) return false;
	Move->SetUpdatedComponent(Character->GetCapsuleComponent());
	Move->MovementMode = MOVE_Walking;

	UGGYGOMovementSet* Set = NewObject<UGGYGOMovementSet>(Character);
	Set->bUseCurveDrivenSpeed = true;
	Set->WalkSpeed = 64.0f;
	Set->RunSpeed = 128.0f;
	Set->RootMotionScale = 1.0f;
	Set->WalkToRunHoldSeconds = 2.0f;
	Set->WalkStartProfile = MakeProfile(Set, false, 64.0f, 64.0f);
	Set->WalkLoopProfile = MakeProfile(Set, true, 64.0f, 64.0f);
	Set->RunLoopProfile = MakeProfile(Set, true, 128.0f, 128.0f);
	Set->StartStopProfile = MakeProfile(Set, false, 64.0f, 0.0f);
	Set->WalkStopProfile = MakeProfile(Set, false, 64.0f, 0.0f);
	Set->RunStopProfile = MakeProfile(Set, false, 128.0f, 0.0f);
	Set->TurnBackProfile = MakeProfile(Set, false, 128.0f, 0.0f, 180.0f);
	if (!TestTrue(TEXT("Seven healthy consumer Profiles bind"), BindMovementFixture(Move, Set))) return false;

	FConsumerBindingCleanup ColdCleanup{*this, Move, {},
		TStrongObjectPtr<UObject>(NewObject<UInputAction>(Move, NAME_None, RF_Transient)), TEXT("Cold")};
	FGGYGOMovementInputSessionIdentity Session;
	if (!Bind(ColdCleanup, Session)) return false;

	FGGYGOMovementInputFact Fact = MakeFact(Session, EKind::SessionOpened, 0, 1,
		EMode::Invalid, EProof::Invalid, NAME_None);
	if (!Consume(Move, ColdCleanup.Binding, Fact, EResult::Rejected, false, TEXT("Opened missing Mode"))) return false;
	Fact = MakeFact(Session, EKind::SessionOpened, 0, 2, EMode::Cold, EProof::ColdPhysicalPress, NAME_None);
	if (!Consume(Move, ColdCleanup.Binding, Fact, EResult::Rejected, false, TEXT("Opened carries wrong Proof"))) return false;
	const FGGYGOMovementInputFact Opened = MakeFact(Session, EKind::SessionOpened, 0, 3,
		EMode::Cold, EProof::Invalid, NAME_None);
	if (!Consume(Move, ColdCleanup.Binding, Opened, EResult::Recorded, true, TEXT("Explicit Cold Opened"))) return false;
	Fact = Opened;
	Fact.SessionMode = EMode::Rearm;
	if (!Consume(Move, ColdCleanup.Binding, Fact, EResult::Rejected, false, TEXT("Same event changes legal Mode"))) return false;

	Fact = MakeFact(Session, EKind::RequestStarted, 1, 4, EMode::Invalid, EProof::Invalid, NAME_None);
	if (!Consume(Move, ColdCleanup.Binding, Fact, EResult::Rejected, false, TEXT("Started missing Proof"))) return false;
	Fact = MakeFact(Session, EKind::RequestStarted, 1, 5, EMode::Cold, EProof::ColdPhysicalPress, NAME_None);
	if (!Consume(Move, ColdCleanup.Binding, Fact, EResult::Rejected, false, TEXT("Started carries wrong Mode"))) return false;
	const FGGYGOMovementInputFact FirstStart = MakeFact(Session, EKind::RequestStarted, 1, 6,
		EMode::Invalid, EProof::ColdPhysicalPress, NAME_None);
	if (!Consume(Move, ColdCleanup.Binding, FirstStart, EResult::Recorded, true, TEXT("First Cold Start without Neutral"))) return false;
	if (!EvaluateHealthy(Move, TEXT("Cold admitted request produces a real Before sample"))) return false;

	const FGGYGOLocomotionCurveSample InitialSample = Move->GetCurveMotion();
	const EGGYGOGait InitialGait = Move->GetResolvedGait();
	const EGGYGOLocomotionMotionType InitialMotion = Move->GetLocomotionMotionType();
	const float InitialTime = Move->GetTestMotionTime();
	if (!Consume(Move, ColdCleanup.Binding, FirstStart, EResult::Duplicate, true, TEXT("Exact Cold Start replay"))) return false;
	if (!TestTrue(TEXT("Duplicate retains actual evaluated sample"),
		Move->GetCurveMotion().bHasCurveSource && Move->GetCurveMotion().Speed == InitialSample.Speed)) return false;
	if (!TestEqual(TEXT("Duplicate does not restart native gait"), Move->GetResolvedGait(), InitialGait)) return false;
	if (!TestEqual(TEXT("Duplicate does not restart native motion"), Move->GetLocomotionMotionType(), InitialMotion)) return false;
	if (!TestEqual(TEXT("Duplicate does not restart read-only native time"), Move->GetTestMotionTime(), InitialTime)) return false;
	Fact = FirstStart;
	Fact.StartProof = EProof::ReleasedThenPhysicalPress;
	if (!Consume(Move, ColdCleanup.Binding, Fact, EResult::Rejected, false, TEXT("Same event changes legal Proof"))) return false;

	Fact = MakeFact(Session, EKind::RequestStarted, 1, 100,
		EMode::Invalid, EProof::ReleasedThenPhysicalPress, NAME_None);
	if (!Consume(Move, ColdCleanup.Binding, Fact, EResult::Stale, false, TEXT("Old Request with Event100"))) return false;
	if (!Consume(Move, ColdCleanup.Binding, FirstStart, EResult::Duplicate, true, TEXT("Stale Event100 leaves original replay watermark"))) return false;
	Fact = MakeFact(Session, EKind::RequestReleased, 1, 7, EMode::Invalid, EProof::Invalid, NAME_None);
	if (!Consume(Move, ColdCleanup.Binding, Fact, EResult::Recorded, true, TEXT("Exact Released below rejected Event100"))) return false;
	Fact = MakeFact(Session, EKind::NeutralConfirmed, 0, 8,
		EMode::Invalid, EProof::ReleasedThenPhysicalPress, NAME_None);
	if (!Consume(Move, ColdCleanup.Binding, Fact, EResult::Rejected, false, TEXT("Neutral carries wrong Proof"))) return false;
	Fact = MakeFact(Session, EKind::NeutralConfirmed, 0, 9, EMode::Invalid, EProof::Invalid, NAME_None);
	if (!Consume(Move, ColdCleanup.Binding, Fact, EResult::Recorded, true, TEXT("Explicit Neutral after exact Released"))) return false;
	Fact = MakeFact(Session, EKind::RequestStarted, 2, 10,
		EMode::Invalid, EProof::ColdPhysicalPress, NAME_None);
	if (!Consume(Move, ColdCleanup.Binding, Fact, EResult::Rejected, false, TEXT("Consumed Cold window cannot start again"))) return false;
	Fact = MakeFact(Session, EKind::RequestStarted, 2, 11,
		EMode::Invalid, EProof::ReleasedThenPhysicalPress, NAME_None);
	if (!Consume(Move, ColdCleanup.Binding, Fact, EResult::Recorded, true, TEXT("Rearm in original Cold session"))) return false;
	if (!EvaluateHealthy(Move, TEXT("Released Neutral Rearm produces a healthy Before sample"))) return false;

	AGGYGOLocomotionTestCharacter* UnknownCharacter = World->SpawnActor<AGGYGOLocomotionTestCharacter>();
	if (!TestNotNull(TEXT("Unresolved consumer character"), UnknownCharacter)) return false;
	UGGYGOLocomotionTestMovementComponent* UnknownMove =
		Cast<UGGYGOLocomotionTestMovementComponent>(UnknownCharacter->GetCharacterMovement());
	if (!TestNotNull(TEXT("Unresolved consumer actual CMC"), UnknownMove)) return false;
	UnknownMove->SetUpdatedComponent(UnknownCharacter->GetCapsuleComponent());
	UnknownMove->MovementMode = MOVE_Walking;
	if (!TestTrue(TEXT("Unresolved consumer shares immutable healthy Profiles"), BindMovementFixture(UnknownMove, Set))) return false;
	FConsumerBindingCleanup UnknownCleanup{*this, UnknownMove, {},
		TStrongObjectPtr<UObject>(NewObject<UInputAction>(UnknownMove, NAME_None, RF_Transient)), TEXT("Unresolved")};
	FGGYGOMovementInputSessionIdentity UnknownSession;
	if (!Bind(UnknownCleanup, UnknownSession)) return false;
	Fact = MakeFact(UnknownSession, EKind::SessionOpened, 0, 1, EMode::Cold, EProof::Invalid, NAME_None);
	if (!Consume(UnknownMove, UnknownCleanup.Binding, Fact, EResult::Recorded, true, TEXT("Second explicit Cold Opened"))) return false;
	Fact = MakeFact(UnknownSession, EKind::SourceUnresolved, 0, 2,
		EMode::Invalid, EProof::Invalid, FName(TEXT("UnresolvedBeforeFirstRequest")));
	if (!Consume(UnknownMove, UnknownCleanup.Binding, Fact, EResult::Recorded, false, TEXT("Unissued SourceUnresolved"))) return false;
	Fact = MakeFact(UnknownSession, EKind::RequestStarted, 1, 3,
		EMode::Invalid, EProof::ColdPhysicalPress, NAME_None);
	if (!Consume(UnknownMove, UnknownCleanup.Binding, Fact, EResult::Rejected, false, TEXT("Unresolved closes unused Cold window"))) return false;
	Fact = MakeFact(UnknownSession, EKind::RequestStarted, 1, 4,
		EMode::Invalid, EProof::ReleasedThenPhysicalPress, NAME_None);
	if (!Consume(UnknownMove, UnknownCleanup.Binding, Fact, EResult::Rejected, false, TEXT("Unresolved does not fabricate Neutral"))) return false;
	Fact = MakeFact(UnknownSession, EKind::RequestReleased, 1, 5, EMode::Invalid, EProof::Invalid, NAME_None);
	if (!Consume(UnknownMove, UnknownCleanup.Binding, Fact, EResult::Stale, false, TEXT("Unresolved does not issue a source request"))) return false;
	Fact = MakeFact(UnknownSession, EKind::NeutralConfirmed, 0, 6, EMode::Invalid, EProof::Invalid, NAME_None);
	if (!Consume(UnknownMove, UnknownCleanup.Binding, Fact, EResult::Recorded, true, TEXT("Explicit Neutral after unissued Unresolved"))) return false;
	Fact = MakeFact(UnknownSession, EKind::RequestStarted, 1, 7,
		EMode::Invalid, EProof::ReleasedThenPhysicalPress, NAME_None);
	if (!Consume(UnknownMove, UnknownCleanup.Binding, Fact, EResult::Recorded, true, TEXT("First Rearm after unissued Unresolved"))) return false;
	if (!EvaluateHealthy(UnknownMove, TEXT("Unresolved recovery produces a healthy Before sample"))) return false;

	// No FAILED injection or expected execution errors: only consumer protocol is exercised.
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOLocomotionFailedRequestRecoveryTest,
	"GGYGO.Movement.Locomotion.InputConsumer.FailedRequestRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOLocomotionFailedRequestRecoveryTest::RunTest(const FString& Parameters)
{
	UWorld::InitializationValues Init;
	Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
		.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("Failed recovery world"), World)) return false;
	struct FRecoveryWorldCleanup
	{
		UWorld* World;
		~FRecoveryWorldCleanup() { World->DestroyWorld(false); }
	} WorldCleanup{World};

	AGGYGOLocomotionTestCharacter* Character = World->SpawnActor<AGGYGOLocomotionTestCharacter>();
	if (!TestNotNull(TEXT("Failed recovery character"), Character)) return false;
	UGGYGOLocomotionTestMovementComponent* Move =
		Cast<UGGYGOLocomotionTestMovementComponent>(Character->GetCharacterMovement());
	if (!TestNotNull(TEXT("Failed recovery actual CMC"), Move)) return false;
	Move->SetUpdatedComponent(Character->GetCapsuleComponent());
	Move->MovementMode = MOVE_Walking;

	// Both sets are fully authored before the first bind and remain immutable.
	UGGYGOMovementSet* HealthySet = NewObject<UGGYGOMovementSet>(Character, FName(TEXT("FailedRecoveryHealthySet")));
	HealthySet->bUseCurveDrivenSpeed = true;
	HealthySet->WalkSpeed = 64.0f;
	HealthySet->RunSpeed = 128.0f;
	HealthySet->RootMotionScale = 1.0f;
	HealthySet->WalkToRunHoldSeconds = 2.0f;
	HealthySet->WalkStartProfile = MakeProfile(HealthySet, false, 64.0f, 64.0f);
	HealthySet->WalkLoopProfile = MakeProfile(HealthySet, true, 64.0f, 64.0f);
	HealthySet->RunLoopProfile = MakeProfile(HealthySet, true, 128.0f, 128.0f);
	HealthySet->StartStopProfile = MakeProfile(HealthySet, false, 64.0f, 0.0f);
	HealthySet->WalkStopProfile = MakeProfile(HealthySet, false, 64.0f, 0.0f);
	HealthySet->RunStopProfile = MakeProfile(HealthySet, false, 128.0f, 0.0f);
	HealthySet->TurnBackProfile = MakeProfile(HealthySet, false, 128.0f, 0.0f, 180.0f);

	UGGYGOMovementSet* BadSet = NewObject<UGGYGOMovementSet>(Character, FName(TEXT("FailedRecoveryBadSet")));
	BadSet->bUseCurveDrivenSpeed = true;
	BadSet->WalkSpeed = HealthySet->WalkSpeed;
	BadSet->RunSpeed = HealthySet->RunSpeed;
	BadSet->RootMotionScale = HealthySet->RootMotionScale;
	BadSet->WalkToRunHoldSeconds = HealthySet->WalkToRunHoldSeconds;
	BadSet->WalkLoopProfile = HealthySet->WalkLoopProfile;
	BadSet->RunLoopProfile = HealthySet->RunLoopProfile;
	BadSet->StartStopProfile = HealthySet->StartStopProfile;
	BadSet->WalkStopProfile = HealthySet->WalkStopProfile;
	BadSet->RunStopProfile = HealthySet->RunStopProfile;
	BadSet->TurnBackProfile = HealthySet->TurnBackProfile;
	UGGYGOLocomotionMotionProfile* BadStart =
		NewObject<UGGYGOLocomotionMotionProfile>(BadSet, FName(TEXT("FailedRecoveryBadWalkStart")));
	BadStart->Duration = 1.0f;
	BadStart->bLoop = false;
	AddLinearKeys(BadStart->DirectionXCurve, 1.0f, 1.0f);
	AddLinearKeys(BadStart->DirectionYCurve, 0.0f, 0.0f);
	AddLinearKeys(BadStart->YawCurve, 0.0f, 0.0f);
	FRichCurveKey First(0.0f, 1.0f);
	FRichCurveKey Last(1.0f, 1.0f);
	First.InterpMode = Last.InterpMode = RCIM_Cubic;
	First.TangentMode = Last.TangentMode = RCTM_User;
	First.TangentWeightMode = Last.TangentWeightMode = RCTWM_WeightedNone;
	First.ArriveTangent = First.LeaveTangent = -8.0f;
	Last.ArriveTangent = Last.LeaveTangent = 8.0f;
	FRichCurve* BadSpeed = BadStart->SpeedCurve.GetRichCurve();
	BadSpeed->SetKeys(TArray<FRichCurveKey>{First, Last});
	BadSet->WalkStartProfile = BadStart;
	if (!TestTrue(TEXT("Bad fixture retains positive keys and authored cubic User tangents"),
		BadSpeed->GetFirstKey().Time == 0.0f && BadSpeed->GetLastKey().Time == 1.0f
			&& BadSpeed->GetFirstKey().Value == 1.0f && BadSpeed->GetLastKey().Value == 1.0f
			&& BadSpeed->GetFirstKey().InterpMode == RCIM_Cubic
			&& BadSpeed->GetLastKey().InterpMode == RCIM_Cubic
			&& BadSpeed->GetFirstKey().TangentMode == RCTM_User
			&& BadSpeed->GetLastKey().TangentMode == RCTM_User
			&& BadSpeed->GetFirstKey().TangentWeightMode == RCTWM_WeightedNone
			&& BadSpeed->GetLastKey().TangentWeightMode == RCTWM_WeightedNone
			&& BadSpeed->GetFirstKey().LeaveTangent == -8.0f
			&& BadSpeed->GetLastKey().ArriveTangent == 8.0f)) return false;
	if (!TestEqual(TEXT("Native cubic first interval ends at positive speed"),
		BadSpeed->Eval(0.1f), 0.28f, 0.0001f)) return false;
	if (!TestEqual(TEXT("Native cubic second interval ends at negative speed"),
		BadSpeed->Eval(0.2f), -0.28f, 0.0001f)) return false;
	FString ValidationError = TEXT("previous validation error");
	if (!TestTrue(TEXT("Positive-key cubic profile passes static validation"),
		BadStart->ValidateProfile(ValidationError))) return false;
	if (!TestTrue(TEXT("Valid cubic profile clears validation error"), ValidationError.IsEmpty())) return false;

	const auto BindSet = [this, Move](const UGGYGOMovementSet* Set, const TCHAR* Stage)
	{
		FString Error;
		const bool bAccepted = BindMovementFixture(Move, Set, &Error);
		return TestTrue(FString::Printf(TEXT("%s accepts immutable configuration (Error='%s')"), Stage, *Error),
			bAccepted && Error.IsEmpty());
	};
	if (!BindSet(HealthySet, TEXT("Initial healthy set"))) return false;

	// Synthetic source facts exercise public admission; CMC alone issues execution requests.
	struct FRecoveryBindingCleanup
	{
		FAutomationTestBase& Test;
		TWeakObjectPtr<UGGYGOCharacterMovementComponent> Movement;
		FGGYGOMovementInputConsumerBindingId Binding;
		TStrongObjectPtr<UObject> SourceMarker;
		bool bClosed = false;

		bool Close()
		{
			if (bClosed) return true;
			bClosed = true;
			bool bSucceeded = true;
			if (Binding.ConsumerBindingSerial != 0)
			{
				UGGYGOCharacterMovementComponent* Consumer = Movement.Get();
				if (Test.TestNotNull(TEXT("Failed recovery cleanup consumer"), Consumer))
				{
					FString Error;
					const bool bInvalidated = Consumer->InvalidateMovementInputSession(
						Binding, FName(TEXT("FailedRequestRecoveryTestEnded")), Error);
					bSucceeded = Test.TestTrue(FString::Printf(
						TEXT("Failed recovery invalidates original binding (Error='%s')"), *Error),
						bInvalidated && Error.IsEmpty());
				}
				else
				{
					bSucceeded = false;
				}
			}
			SourceMarker.Reset();
			return bSucceeded;
		}
		~FRecoveryBindingCleanup() { Close(); }
	} BindingCleanup{*this, Move, {},
		TStrongObjectPtr<UObject>(NewObject<UInputAction>(Move, NAME_None, RF_Transient))};
	if (!TestNotNull(TEXT("Failed recovery concrete source identity"), BindingCleanup.SourceMarker.Get())) return false;
	FGGYGOMovementInputSessionIdentity Session;
	Session.Producer = BindingCleanup.SourceMarker.Get();
	Session.SessionSerial = 1;
	FString BindingError;
	if (!TestTrue(TEXT("Failed recovery public Bind"),
		Move->BindMovementInputSession(Session, Move->GetMovementInputBindingSerial(),
			BindingCleanup.Binding, BindingError))) return false;
	if (!TestTrue(TEXT("Failed recovery Bind clears error"), BindingError.IsEmpty())) return false;
	if (!TestTrue(TEXT("Failed recovery retains original binding identities"),
		BindingCleanup.Binding.Consumer.Get() == Move && BindingCleanup.Binding.ConsumerBindingSerial != 0
			&& BindingCleanup.Binding.SourceSession == Session)) return false;

	using EKind = EGGYGOMovementInputFactKind;
	using EMode = EGGYGOMovementInputSessionMode;
	using EProof = EGGYGOMovementInputStartProof;
	using EResult = EGGYGOMovementInputConsumeResult;
	const auto MakeFact = [&Session](EKind Kind, uint64 RequestSerial, uint64 EventSerial, EMode Mode, EProof Proof)
	{
		FGGYGOMovementInputFact Fact;
		Fact.Request.Session = Session;
		Fact.Request.RequestSerial = RequestSerial;
		Fact.EventSerial = EventSerial;
		Fact.Kind = Kind;
		Fact.SessionMode = Mode;
		Fact.StartProof = Proof;
		Fact.Reason = NAME_None;
		return Fact;
	};
	const auto Consume = [this, Move, &BindingCleanup](const FGGYGOMovementInputFact& Fact,
		EResult Expected, const TCHAR* Stage)
	{
		FString Error;
		const EResult Actual = Move->ConsumeMovementInputFact(BindingCleanup.Binding, Fact, Error);
		return TestTrue(FString::Printf(TEXT("%s Result=%u Expected=%u (Error='%s')"), Stage,
			static_cast<uint32>(Actual), static_cast<uint32>(Expected), *Error),
			Actual == Expected && Error.IsEmpty());
	};
	const auto CheckBlocked = [this, Move](float ExpectedTime, const TCHAR* Stage)
	{
		const FGGYGOLocomotionCurveSample& Sample = Move->GetCurveMotion();
		if (!TestTrue(FString(Stage) + TEXT(" fully clears sample"),
			!Sample.bHasCurveSource && !Sample.bHasAuthoredDirection && !Sample.bHasPositionDelta && !Sample.bLoopClip
				&& Sample.Speed == 0.0f && Sample.YawDeltaDegrees == 0.0f && Sample.YawTotalDegrees == 0.0f
				&& Sample.PositionDelta.IsZero() && Sample.Velocity.IsZero() && Sample.Direction.IsZero()
				&& Sample.DirectionAngle == 0.0f && Sample.ClipLength == 0.0f)) return false;
		if (!TestFalse(FString(Stage) + TEXT(" rejects curve qualification"), Move->IsCurveDrivingSpeed())) return false;
		if (!TestEqual(FString(Stage) + TEXT(" rejects speed without fixed fallback"), Move->GetMaxSpeed(), 0.0f)) return false;
		return TestEqual(FString(Stage) + TEXT(" retains allowed native time"), Move->GetTestMotionTime(), ExpectedTime);
	};
	const auto BeforeBlocked = [this, Move, &CheckBlocked](const TCHAR* Stage)
	{
		const float Time = Move->GetTestMotionTime();
		const uint16 Sequence = Move->GetTestSequence();
		const EGGYGOLocomotionMotionType Motion = Move->GetLocomotionMotionType();
		const EGGYGOGait Gait = Move->GetResolvedGait();
		Move->UpdateCharacterStateBeforeMovement(0.1f);
		if (!CheckBlocked(Time, Stage)) return false;
		if (!TestEqual(FString(Stage) + TEXT(" cannot restart sequence"), Move->GetTestSequence(), Sequence)) return false;
		if (!TestEqual(FString(Stage) + TEXT(" cannot restart motion"), Move->GetLocomotionMotionType(), Motion)) return false;
		return TestEqual(FString(Stage) + TEXT(" cannot restart gait"), Move->GetResolvedGait(), Gait);
	};
	const auto EvaluateHealthy = [this, Move](const TCHAR* Stage)
	{
		Move->SetTestAcceleration(FVector::ForwardVector);
		Move->UpdateCharacterStateBeforeMovement(0.016f);
		const FGGYGOLocomotionCurveSample& Sample = Move->GetCurveMotion();
		return TestTrue(Stage, Sample.bHasCurveSource && FMath::IsFinite(Sample.Speed) && Sample.Speed == 64.0f
			&& Move->IsCurveDrivingSpeed() && Move->GetMaxSpeed() == 64.0f
			&& Move->GetResolvedGait() == EGGYGOGait::Walk
			&& Move->GetLocomotionMotionType() == EGGYGOLocomotionMotionType::WalkStart
			&& FMath::IsNearlyEqual(Move->GetTestMotionTime(), 0.016f, 0.000001f));
	};
	const auto FailOnSecondInterval = [this, Move, BadSpeed, &CheckBlocked](const TCHAR* Stage)
	{
		Move->SetTestAcceleration(FVector::ForwardVector);
		if (!TestEqual(FString(Stage) + TEXT(" starts after legal reset"), Move->GetTestMotionTime(), 0.0f)) return false;
		Move->UpdateCharacterStateBeforeMovement(0.1f);
		if (!TestTrue(FString(Stage) + TEXT(" first native interval genuinely succeeds"),
			Move->GetCurveMotion().bHasCurveSource && Move->IsCurveDrivingSpeed()
				&& Move->GetResolvedGait() == EGGYGOGait::Walk
				&& Move->GetLocomotionMotionType() == EGGYGOLocomotionMotionType::WalkStart)) return false;
		if (!TestEqual(FString(Stage) + TEXT(" commits first native time"), Move->GetTestMotionTime(), 0.1f)) return false;
		if (!TestEqual(FString(Stage) + TEXT(" consumes authored positive speed"),
			Move->GetCurveMotion().Speed, BadSpeed->Eval(0.1f), 0.0001f)) return false;
		if (!TestEqual(FString(Stage) + TEXT(" exposes authored positive max speed"),
			Move->GetMaxSpeed(), BadSpeed->Eval(0.1f), 0.0001f)) return false;
		const float Time = Move->GetTestMotionTime();
		const uint16 Sequence = Move->GetTestSequence();
		const EGGYGOLocomotionMotionType Motion = Move->GetLocomotionMotionType();
		const EGGYGOGait Gait = Move->GetResolvedGait();
		// The production SingleInterval failure emits its original Error and latches FAILED.
		Move->UpdateCharacterStateBeforeMovement(0.1f);
		if (!CheckBlocked(Time, Stage)) return false;
		if (!TestEqual(FString(Stage) + TEXT(" cannot commit failed candidate sequence"), Move->GetTestSequence(), Sequence)) return false;
		if (!TestEqual(FString(Stage) + TEXT(" cannot commit failed candidate motion"), Move->GetLocomotionMotionType(), Motion)) return false;
		return TestEqual(FString(Stage) + TEXT(" cannot commit failed candidate gait"), Move->GetResolvedGait(), Gait);
	};

	const FGGYGOMovementInputFact Opened = MakeFact(EKind::SessionOpened, 0, 1, EMode::Cold, EProof::Invalid);
	const FGGYGOMovementInputFact FirstStart = MakeFact(EKind::RequestStarted, 1, 2, EMode::Invalid, EProof::ColdPhysicalPress);
	if (!Consume(Opened, EResult::Recorded, TEXT("Original Cold session opens"))) return false;
	if (!Consume(FirstStart, EResult::Recorded, TEXT("Healthy source request1 starts"))) return false;
	if (!EvaluateHealthy(TEXT("Issued request1 yields real healthy Before sample"))) return false;

	if (!BindSet(BadSet, TEXT("Request1 bad set passes static binding"))) return false;
	if (!FailOnSecondInterval(TEXT("Real failed request1"))) return false;
	if (!BeforeBlocked(TEXT("Request1 same held remains failed"))) return false;
	if (!BindSet(HealthySet, TEXT("Repair request1 configuration and public Reset"))) return false;
	if (!CheckBlocked(0.0f, TEXT("Legal configuration cleanup resets time"))) return false;
	if (!BeforeBlocked(TEXT("Healthy configuration cannot retry request1"))) return false;
	if (!Consume(FirstStart, EResult::Duplicate, TEXT("Exact Started replay cannot reissue request1"))) return false;
	if (!BeforeBlocked(TEXT("Duplicate Started leaves request1 failed"))) return false;
	Move->SetTestAcceleration(FVector::ZeroVector);
	if (!BeforeBlocked(TEXT("Zero acceleration cannot clear request1 failure"))) return false;
	Move->SetTestAcceleration(FVector::ForwardVector);
	if (!BeforeBlocked(TEXT("Restored held acceleration cannot retry request1"))) return false;
	if (!BindSet(HealthySet, TEXT("Repeated public configuration Reset"))) return false;
	if (!CheckBlocked(0.0f, TEXT("Repeated Reset legally clears native time"))) return false;
	if (!BeforeBlocked(TEXT("Repeated Reset cannot rearm request1"))) return false;
	FGGYGOMovementInputConsumerBindingId SameSessionBinding;
	BindingError = TEXT("previous binding error");
	if (!TestTrue(TEXT("Same-session public Bind remains idempotent"),
		Move->BindMovementInputSession(Session, Move->GetMovementInputBindingSerial(),
			SameSessionBinding, BindingError))) return false;
	if (!TestTrue(TEXT("Same-session Bind retains original grant and clears error"),
		SameSessionBinding == BindingCleanup.Binding && BindingError.IsEmpty()
			&& Move->GetMovementInputBindingSerial() == BindingCleanup.Binding.ConsumerBindingSerial)) return false;
	if (!BeforeBlocked(TEXT("Same-session Bind cannot clear request1 failure"))) return false;

	if (!Consume(MakeFact(EKind::RequestReleased, 1, 3, EMode::Invalid, EProof::Invalid),
		EResult::Recorded, TEXT("Exact failed request1 Released"))) return false;
	if (!BeforeBlocked(TEXT("Released1 alone cannot clear failure"))) return false;
	if (!Consume(MakeFact(EKind::NeutralConfirmed, 0, 4, EMode::Invalid, EProof::Invalid),
		EResult::Recorded, TEXT("Neutral after exact Released1"))) return false;
	if (!BeforeBlocked(TEXT("Released1 Neutral alone cannot retry"))) return false;
	if (!BindSet(BadSet, TEXT("Immutable bad set before new request2"))) return false;
	if (!BeforeBlocked(TEXT("Bad configuration rebind alone cannot retry"))) return false;
	if (!Consume(MakeFact(EKind::RequestStarted, 2, 5, EMode::Invalid, EProof::ReleasedThenPhysicalPress),
		EResult::Recorded, TEXT("Qualified source request2 starts against bad configuration"))) return false;
	if (!FailOnSecondInterval(TEXT("Real failed request2"))) return false;

	if (!BindSet(HealthySet, TEXT("Repair request2 configuration"))) return false;
	if (!CheckBlocked(0.0f, TEXT("Request2 repair legally resets time"))) return false;
	if (!BeforeBlocked(TEXT("Healthy configuration still cannot retry request2"))) return false;
	if (!Consume(MakeFact(EKind::RequestReleased, 2, 6, EMode::Invalid, EProof::Invalid),
		EResult::Recorded, TEXT("Exact failed request2 Released"))) return false;
	if (!BeforeBlocked(TEXT("Released2 alone cannot clear failure"))) return false;
	if (!Consume(MakeFact(EKind::NeutralConfirmed, 0, 7, EMode::Invalid, EProof::Invalid),
		EResult::Recorded, TEXT("Neutral after exact Released2"))) return false;
	if (!BeforeBlocked(TEXT("Released2 Neutral alone cannot retry"))) return false;
	if (!Consume(MakeFact(EKind::RequestStarted, 3, 8, EMode::Invalid, EProof::ReleasedThenPhysicalPress),
		EResult::Recorded, TEXT("Qualified healthy source request3 starts"))) return false;
	if (!EvaluateHealthy(TEXT("Only qualified request3 restores real healthy sample and speed"))) return false;

	const FString ConsumerPath = Move->GetPathName();
	const FString OwnerPath = Character->GetPathName();
	const FString ProducerPath = BindingCleanup.SourceMarker.Get()->GetPathName();
	const FString BadSetPath = BadSet->GetPathName();
	const FString BadProfilePath = BadStart->GetPathName();
	const uint64 OriginalBindingSerial = BindingCleanup.Binding.ConsumerBindingSerial;
	if (!BindingCleanup.Close()) return false;
	// Automation must retain Fail from the two real Error events; this is a completion marker only.
	AddInfo(FString::Printf(TEXT("GGYGO_FAILED_REQUEST_RECOVERY_COMPLETE Consumer='%s' Owner='%s' Producer='%s' Session=1 Binding=%llu BadSet='%s' BadWalkStart='%s' ExpectedFailurePairs='1/1,2/2' RecoveredInputRequest=3"),
		*ConsumerPath, *OwnerPath, *ProducerPath, static_cast<unsigned long long>(OriginalBindingSerial),
		*BadSetPath, *BadProfilePath));
	return true;
}

#endif

#include "Character/Tests/GGYGOLocomotionMovementTestTypes.h"
#include "Character/Data/GGYGOLocomotionSteeringEvaluation.h"
#include "Character/Data/GGYGOMovementSet.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "InputAction.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOLocomotionSteeringTest,
	"GGYGO.Movement.Locomotion.Steering.NativeInterval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOLocomotionSteeringTest::RunTest(const FString& Parameters)
{
	UGGYGOMovementSet* Set = NewObject<UGGYGOMovementSet>();
	Set->bUseCurveDrivenSpeed = false; // Explicit fixed-speed fixture, not missing-curve fallback.
	Set->bEnableLocomotionSteering = true;
	Set->WalkToRunHoldSeconds = 60.0f;
	Set->SteeringMinYawRate = 90.0f;
	Set->SteeringMaxYawRate = 360.0f;
	FString Error;
	TestFalse(TEXT("Enabled steering rejects missing required response curve"), Set->ValidateMovementSet(Error));
	TestTrue(TEXT("Missing response diagnosis identifies configuration"), Error.Contains(TEXT("LocomotionSteering")) && Error.Contains(TEXT("curve")));
	FRichCurve* Curve = Set->SteeringAngleResponse.GetRichCurve();
	Curve->SetKeyInterpMode(Curve->AddKey(0.0f, 0.0f), RCIM_Linear);
	Curve->SetKeyInterpMode(Curve->AddKey(180.0f, 1.0f), RCIM_Linear);
	if (!TestTrue(TEXT("Author-defined response configuration validates"), Set->ValidateMovementSet(Error))) return false;
	float WalkBudget = 0.0f, MidBudget = 0.0f, RunBudget = 0.0f;
	using namespace GGYGOLocomotionSteeringEvaluation;
	if (!EvaluateAngularBudget(*Set, 90.0f, 0.0f, 0.05f, WalkBudget, Error)
		|| !EvaluateAngularBudget(*Set, 90.0f, 0.5f, 0.05f, MidBudget, Error)
		|| !EvaluateAngularBudget(*Set, 90.0f, 1.0f, 0.05f, RunBudget, Error))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Original continuous Alpha gives weaker Walk and stronger Run curvature"), RunBudget < MidBudget && MidBudget < WalkBudget);
	const FVector NativeResult(0.0, 200.0, 13.0);
	const FVector Limited = ConstrainPlanarHeading(FVector(200.0, 0.0, 13.0), NativeResult, 0.0f, WalkBudget);
	TestTrue(TEXT("Angular budget preserves native planar speed and Z"), FMath::IsNearlyEqual(Limited.Size2D(), NativeResult.Size2D(), 0.001) && Limited.Z == NativeResult.Z);
	float TrajectoryRate = 0.0f;
	TestTrue(TEXT("Real trajectory derivative can oppose capsule correction"),
		CalculateVelocityYawRate(FRotator(0.0f, 30.0f, 0.0f).Vector() * 200.0,
			FRotator(0.0f, 40.0f, 0.0f).Vector() * 200.0, 0.05f, TrajectoryRate)
		&& TrajectoryRate > 0.0f && FMath::FindDeltaAngleDegrees(60.0f, 40.0f) < 0.0f);
	TestFalse(TEXT("Zero original velocity does not invent observed trajectory rate"),
		CalculateVelocityYawRate(FVector::ZeroVector, NativeResult, 0.05f, TrajectoryRate));

	UWorld::InitializationValues Init;
	Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
		.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("Native steering world"), World)) return false;
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AGGYGOLocomotionTestCharacter* Character = World->SpawnActor<AGGYGOLocomotionTestCharacter>();
	if (!TestNotNull(TEXT("Original native character"), Character)) return false;
	UGGYGOLocomotionTestMovementComponent* Move = Cast<UGGYGOLocomotionTestMovementComponent>(Character->GetCharacterMovement());
	if (!TestNotNull(TEXT("Original movement component"), Move)) return false;
	TestTrue(TEXT("Missing Set with no original initializer is Invalid, not perpetual Initial"),
		Move->GetLocomotionSteeringSnapshot().Status == EGGYGOLocomotionSteeringStatus::Invalid);
	UGGYGOPawnExtensionComponent* Extension = NewObject<UGGYGOPawnExtensionComponent>(Character);
	Character->AddInstanceComponent(Extension); // Real pre-registration source, before this native actor begins play.
	const auto PendingConfiguration = Move->GetLocomotionSteeringSnapshot();
	TestTrue(TEXT("Original native pre-BeginPlay initializer exposes pending configuration, not failed or fabricated Valid"),
		!Character->HasActorBegunPlay() && PendingConfiguration.Status == EGGYGOLocomotionSteeringStatus::Initial
		&& PendingConfiguration.CompletedIntervalSerial == 0 && !PendingConfiguration.bHasVelocityYawRate
		&& PendingConfiguration.OriginalMovement.Get() == Move && PendingConfiguration.OriginalCharacter.Get() == Character
		&& PendingConfiguration.OriginalUpdatedComponent.Get() == Move->UpdatedComponent);
	TestFalse(TEXT("Explicit missing Set submission is rejected"), Move->SetMovementSet(nullptr));
	TestTrue(TEXT("Submitted missing Set closes startup even before BeginPlay"),
		Move->GetLocomotionSteeringSnapshot().Status == EGGYGOLocomotionSteeringStatus::Invalid);
	Extension->DestroyComponent(); // Only needed for the real pre-registration boundary; retain the original standalone movement fixture.
	if (!TestTrue(TEXT("Valid steering Set is accepted through original configuration entry"), Move->SetMovementSet(Set, &Error))) return false;
	const auto Initial = Move->GetLocomotionSteeringSnapshot();
	TestTrue(TEXT("Initial retains original identities without a fake completed sample"),
		Initial.Status == EGGYGOLocomotionSteeringStatus::Initial && Initial.CompletedIntervalSerial == 0
		&& Initial.OriginalMovement.Get() == Move && Initial.OriginalCharacter.Get() == Character
		&& Initial.OriginalUpdatedComponent.Get() == Move->UpdatedComponent);

	// Synthetic upstream facts exercise the public consumer, not HID/PlayerInput issuance.
	UInputAction* Producer = NewObject<UInputAction>(Move);
	FGGYGOMovementInputSessionIdentity Session;
	Session.Producer = Producer; Session.SessionSerial = 1;
	FGGYGOMovementInputConsumerBindingId Binding;
	if (!TestTrue(TEXT("Native fixture binds original input session"),
		Move->BindMovementInputSession(Session, Move->GetMovementInputBindingSerial(), Binding, Error))) return false;
	ON_SCOPE_EXIT
	{
		FString CleanupError;
		TestTrue(TEXT("Original fixture binding retires"), Move->InvalidateMovementInputSession(Binding, FName(TEXT("SteeringTestEnded")), CleanupError));
	};
	uint64 EventSerial = 0;
	const auto Emit = [&](EGGYGOMovementInputFactKind Kind, uint64 RequestSerial)
	{
		FGGYGOMovementInputFact Fact;
		Fact.Request.Session = Session; Fact.Request.RequestSerial = RequestSerial;
		Fact.EventSerial = ++EventSerial; Fact.Kind = Kind;
		Fact.SessionMode = Kind == EGGYGOMovementInputFactKind::SessionOpened
			? EGGYGOMovementInputSessionMode::Rearm : EGGYGOMovementInputSessionMode::Invalid;
		Fact.StartProof = Kind == EGGYGOMovementInputFactKind::RequestStarted
			? EGGYGOMovementInputStartProof::ReleasedThenPhysicalPress : EGGYGOMovementInputStartProof::Invalid;
		return TestTrue(TEXT("Original input fact is recorded"),
			Move->ConsumeMovementInputFact(Binding, Fact, Error) == EGGYGOMovementInputConsumeResult::Recorded && Error.IsEmpty());
	};
	if (!Emit(EGGYGOMovementInputFactKind::SessionOpened, 0)
		|| !Emit(EGGYGOMovementInputFactKind::NeutralConfirmed, 0)
		|| !Emit(EGGYGOMovementInputFactKind::RequestStarted, 1)) return false;
	AActor* FloorOwner = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Native floor owner"), FloorOwner)) return false;
	UBoxComponent* Floor = NewObject<UBoxComponent>(FloorOwner);
	FloorOwner->SetRootComponent(Floor);
	Floor->SetBoxExtent(FVector(1000.0, 1000.0, 20.0));
	Floor->SetCollisionProfileName(TEXT("BlockAll")); Floor->RegisterComponent();
	Floor->SetWorldLocation(FVector(10000.0, 0.0, -20.0));
	Character->SetActorLocation(FVector(10000.0, 0.0, Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.15));
	Move->SetMovementMode(MOVE_Falling); Move->SetMovementMode(MOVE_Walking);
	Move->bRunPhysicsWithNoController = true;
	Move->MoveAutonomous(0.0f, 0.05f, 0, FVector(2048.0, 0.0, 0.0));
	const FVector OldVelocity = Move->Velocity;
	const FVector TurnStart = Character->GetActorLocation();
	const auto BeforeTurn = Move->GetLocomotionSteeringSnapshot();
	Move->MoveAutonomous(0.05f, 0.05f, 0, FVector(0.0, 2048.0, 0.0));
	const FVector ActualVelocity = Move->Velocity;
	const auto Completed = Move->GetLocomotionSteeringSnapshot();
	TestTrue(TEXT("Native velocity follows a curved trajectory instead of snapping 90 degrees"),
		Move->IsMovingOnGround() && OldVelocity.X > 0.0 && ActualVelocity.X > 0.0 && ActualVelocity.Y > 0.0
		&& ActualVelocity.Rotation().Yaw < 45.0f && Character->GetActorLocation().X > TurnStart.X
		&& Character->GetActorLocation().Y > TurnStart.Y);
	TestTrue(TEXT("Capsule follows the actual velocity during the same native interval"),
		Character->GetActorRotation().Yaw > 0.0f
		&& Character->GetActorRotation().Yaw <= ActualVelocity.Rotation().Yaw + 0.1f);
	TestTrue(TEXT("Only completed native interval certifies both derivatives"),
		Completed.Status == EGGYGOLocomotionSteeringStatus::Valid && Completed.bHasVelocityYawRate
		&& Completed.CompletedIntervalSerial > BeforeTurn.CompletedIntervalSerial
		&& FMath::IsNearlyEqual(Completed.NativeDeltaSeconds, 0.05f)
		&& Completed.ActualSignedYawRate > 0.0f && Completed.ActualSignedVelocityYawRate > 0.0f);
	if (!Emit(EGGYGOMovementInputFactKind::RequestReleased, 1)) return false;
	TestTrue(TEXT("Released control cannot keep the old turn effect valid"),
		Move->GetLocomotionSteeringSnapshot().Status == EGGYGOLocomotionSteeringStatus::NotApplicable);
	Move->StopMovementImmediately();
	const auto Stopped = Move->GetLocomotionSteeringSnapshot();
	TestTrue(TEXT("Explicit native stop clears observation origin and derivative"),
		Stopped.SourceEpoch != Completed.SourceEpoch && Stopped.CompletedIntervalSerial == 0 && !Stopped.bHasVelocityYawRate);
	TestFalse(TEXT("Explicit detach remains an unbound configuration decision"), Move->SetMovementSet(nullptr));
	TestTrue(TEXT("Stop/reset cannot disguise actual missing configuration as startup"),
		Move->GetLocomotionSteeringSnapshot().Status == EGGYGOLocomotionSteeringStatus::Invalid);
	return !HasAnyErrors();
}
#endif

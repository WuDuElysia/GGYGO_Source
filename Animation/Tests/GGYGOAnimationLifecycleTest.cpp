/**
 * @file GGYGOAnimationLifecycleTest.cpp
 * @brief Animation 生命周期复位、Pawn 重绑与旧 Locomotion 映射自动化测试
 */
#include "Animation/Tests/GGYGOAnimationLifecycleTestTypes.h"

AGGYGOAnimationLifecycleTestCharacter::AGGYGOAnimationLifecycleTestCharacter(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UGGYGOAnimationLifecycleTestMovementComponent>(
		ACharacter::CharacterMovementComponentName))
{
	AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
}

#if WITH_DEV_AUTOMATION_TESTS

#include "System/GGYGOGameplayTags.h"
#include "Animation/Runtime/GGYGOAnimationStateFrame.h"
#include "Animation/zzzAnim/Data/ZZZAnimSnapshot.h"
#include "Animation/zzzAnim/Data/ZZZAnimStateMemory.h"
#include "Animation/zzzAnim/Data/ZZZAnimTuning.h"
#include "Animation/zzzAnim/Locomotion/ZZZLocomotionEvents.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimSequence.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"

#include <limits>

void UGGYGOAnimationLifecycleTestAnimInstance::SeedDirtyRuntimeStateForTest()
{
	AnimationState.WorldVelocity = FVector(17.0f, 23.0f, 31.0f);
	AnimationState.WorldAcceleration = FVector(3.0f, 4.0f, 5.0f);
	AnimationState.HorizontalSpeed = 123.0f;
	AnimationState.WorldVelocityDirection = FVector(0.5f, 0.5f, 0.0f);
	AnimationState.LocalVelocityAngle = 42.0f;
	AnimationState.LocalVelocityBlend = FVector2D(0.25f, 0.75f);
	AnimationState.WalkRunBlendAlpha = 0.6f;
	AnimationState.bLocomotionSteeringCaptured = true;
	AnimationState.LocomotionSteering.Status = EGGYGOLocomotionSteeringStatus::Valid;
	AnimationState.LocomotionSteering.SourceEpoch = 27;
	AnimationState.LocomotionSteering.CompletedIntervalSerial = 9;
	AnimationState.LocomotionSteering.ActualSignedVelocityYawRate = 123.0f;
	AnimationState.LocomotionSteering.DesiredDirectionError = 42.0f;
	AnimationState.LocomotionSteering.bHasVelocityYawRate = true;
	AnimationState.LocomotionSteering.Diagnostic = TEXT("previous captured interval");
	AnimationState.SteeringExecution.Status = static_cast<uint8>(EGGYGOLocomotionSteeringStatus::Valid);
	AnimationState.SteeringExecution.SourceEpoch = 27;
	AnimationState.SteeringExecution.CompletedIntervalSerial = 9;
	AnimationState.SteeringExecution.CompletedFrame = 101;
	AnimationState.SteeringExecution.NativeDeltaSeconds = 0.016f;
	AnimationState.SteeringExecution.DesiredDirectionError = 42.0f;
	AnimationState.SteeringExecution.ActualSignedYawRate = 120.0f;
	AnimationState.SteeringExecution.ActualSignedVelocityYawRate = 123.0f;
	AnimationState.SteeringExecution.bHasVelocityYawRate = true;
	AnimationState.bCapsuleMeshBasisValid = true;
	AnimationState.LeanAxisCS = -FVector::ForwardVector;
	AnimationState.CapsuleUpCS = FVector::UpVector;
	AnimationState.MotionExecution.Status = EGGYGOMotionExecutionStatus::Valid;
	AnimationState.MotionExecution.SourceKind = EGGYGOMotionExecutionSource::Locomotion;
	AnimationState.MotionExecution.MotionType = EGGYGOLocomotionMotionType::TurnBack;
	AnimationState.MotionExecution.CompletedIntervalSerial = 13;
	AnimationState.MotionExecution.NativeDeltaSeconds = 0.016f;
	AnimationState.MotionExecution.bHasSingleSequenceTime = true;
	AnimationState.MotionExecution.SequenceEndSeconds = 0.75;
	AnimationState.MotionExecution.bEnded = true;
	AnimationState.MotionExecution.EndReason = EGGYGOMotionExecutionEndReason::Completed;
	AnimationState.StopMotionType = EGGYGOStopMotionType::RunStop;
	AnimationState.Gait = EGGYGOGait::Run;
	AnimationState.MovementMode = MOVE_Falling;
	AnimationState.TurnBackPhase = EGGYGOTurnBackPhase::Braking;
	AnimationState.bHasMoveInput = true;
	AnimationState.bMovingHorizontally = true;
	AnimationState.bGrounded = false;
	AnimationState.bMovementBlocked = true;
	AnimationState.bTurnBackRunOut = true;
	AnimationState.OwnedStateTags.AddTag(GGYGOGameplayTags::State_Moving);

	// The production getter is intentionally read-only; the test fixture mutates the
	// non-const AnimInstance subobject only to model stale pre-reset diagnostic data.
	FGGYGOAnimationDebugFrame& Debug = const_cast<FGGYGOAnimationDebugFrame&>(GetAnimationDebugFrame());
	Debug.CurveVelocity = FVector(91.0f, 92.0f, 93.0f);
	Debug.CurveVelocityDirection = FVector(0.0f, 1.0f, 0.0f);
	Debug.CurveVelocityAngle = 77.0f;
	Debug.InputForwardDot = -0.5f;

	StateMemory.GaitBlendY = 0.73f;
	StateMemory.StopValue = 2;

	Snap.Gait = EGGYGOGait::Run;
	Snap.WalkRunBlendAlpha = 0.73f;
	Snap.StopMotionType = EGGYGOStopMotionType::RunStop;
	Snap.bShouldMove = true;
	Snap.AnimBlendX = 0.2f;
	Snap.AnimBlendY = 0.8f;
	Snap.AnimCurveVelocity = FVector(101.0f, 102.0f, 103.0f);
	Snap.AnimCurveVelocityDirection = FVector(0.0f, 1.0f, 0.0f);
	Snap.AnimCurveVelocityAngle = 81.0f;
	Snap.InputForwardDot = -0.8f;
	Snap.TurnBackPhase = EGGYGOTurnBackPhase::Turning;
	Snap.bTurnBackRunOut = true;
	Snap.VelocityLength = 777.0f;
	Snap.ActualVelocityDirection = FVector(1.0f, 0.0f, 0.0f);
	Snap.ActualVelocityBlendX = 0.4f;
	Snap.ActualVelocityBlendY = 0.6f;
	Snap.ActualVelocityAngle = 88.0f;
	Snap.bGrounded = false;
	Snap.bBlockMove = true;

	AnimBlendX = 0.4f;
	AnimBlendY = 0.6f;
	AnimCurveVelocity = FVector(201.0f, 202.0f, 203.0f);
	AnimCurveVelocityDirection = FVector(0.0f, 1.0f, 0.0f);
	AnimCurveVelocityAngle = 89.0f;
	ActualVelocityDirection = FVector(0.0f, 1.0f, 0.0f);
	ActualVelocityBlendX = 0.3f;
	ActualVelocityBlendY = 0.7f;
	ActualVelocityAngle = 90.0f;
	bTurnBackRunOut = true;
	WalkRunLeanAngleDegrees = 7.25f;
	WalkRunLeanAxisCS = -FVector::ForwardVector;
	WalkRunLeanUpCS = FVector::UpVector;
	bWalkRunLeanPresentationValid = true;
	bWalkRunLeanTargetApplicable = true;
	WalkRunLeanFailureReason = TEXT("seeded previous presentation failure");
}

namespace
{
	struct FScopedAnimationTestWorld
	{
		UWorld* World = nullptr;
		UEngine* Engine = nullptr;
		bool bOwnsWorldContext = false;

		FScopedAnimationTestWorld()
		{
			UWorld::InitializationValues Init;
			Init.AllowAudioPlayback(false)
				.RequiresHitProxies(false)
				.CreatePhysicsScene(true)
				.CreateNavigation(false)
				.CreateAISystem(false)
				.ShouldSimulatePhysics(false)
				.SetTransactional(false);
			World = UWorld::CreateWorld(
				EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
			Engine = GEngine;
			if (World && Engine)
			{
				Engine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
				bOwnsWorldContext = true;
			}
		}

		~FScopedAnimationTestWorld()
		{
			if (World)
			{
				// Keep the world context alive through Actor and component teardown.
				World->DestroyWorld(false);
				if (bOwnsWorldContext && Engine)
				{
					Engine->DestroyWorldContext(World);
				}
				World->GetPackage()->SetDirtyFlag(false);
			}
		}
	};

	void TestClearedLifecycleState(
		FAutomationTestBase& Test,
		const UGGYGOAnimationLifecycleTestAnimInstance& AnimInstance,
		const TCHAR* Prefix)
	{
		const FString PrefixString(Prefix);
		const auto Label = [&PrefixString](const TCHAR* Field)
		{
			return FString::Printf(TEXT("%s.%s"), *PrefixString, Field);
		};
		const FGGYGOAnimationStateFrame DefaultFrame;
		const FGGYGOAnimationDebugFrame DefaultDebug;
		const FZZZAnimSnapshot DefaultSnap;
		const FZZZAnimStateMemory DefaultMemory;
		const FGGYGOAnimationStateFrame& Frame = AnimInstance.GetAnimationStateFrame();
		const FGGYGOAnimationDebugFrame& Debug = AnimInstance.GetDebugFrameForTest();
		const FZZZAnimSnapshot& Snap = AnimInstance.GetSnapshotForTest();

#define TEST_FIELD_DEFAULT(Object, Field, DefaultObject) \
		Test.TestEqual(Label(TEXT(#Field)), (Object).Field, (DefaultObject).Field)

		TEST_FIELD_DEFAULT(Frame, WorldVelocity, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, WorldAcceleration, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, HorizontalSpeed, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, WorldVelocityDirection, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, LocalVelocityAngle, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, LocalVelocityBlend, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, WalkRunBlendAlpha, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, bLocomotionSteeringCaptured, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, bCapsuleMeshBasisValid, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, LeanAxisCS, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, CapsuleUpCS, DefaultFrame);
		Test.TestEqual(Label(TEXT("Motion.Status")), static_cast<uint8>(Frame.MotionExecution.Status),
			static_cast<uint8>(DefaultFrame.MotionExecution.Status));
		Test.TestEqual(Label(TEXT("Motion.CompletedSerial")), Frame.MotionExecution.CompletedIntervalSerial, uint64(0));
		Test.TestEqual(Label(TEXT("Motion.NativeDeltaSeconds")), Frame.MotionExecution.NativeDeltaSeconds, 0.0f);
		TEST_FIELD_DEFAULT(Frame.SteeringExecution, Status, DefaultFrame.SteeringExecution);
		TEST_FIELD_DEFAULT(Frame.SteeringExecution, SourceEpoch, DefaultFrame.SteeringExecution);
		TEST_FIELD_DEFAULT(Frame.SteeringExecution, CompletedIntervalSerial, DefaultFrame.SteeringExecution);
		TEST_FIELD_DEFAULT(Frame.SteeringExecution, CompletedFrame, DefaultFrame.SteeringExecution);
		TEST_FIELD_DEFAULT(Frame.SteeringExecution, NativeDeltaSeconds, DefaultFrame.SteeringExecution);
		TEST_FIELD_DEFAULT(Frame.SteeringExecution, DesiredDirectionError, DefaultFrame.SteeringExecution);
		TEST_FIELD_DEFAULT(Frame.SteeringExecution, ActualSignedYawRate, DefaultFrame.SteeringExecution);
		TEST_FIELD_DEFAULT(Frame.SteeringExecution, ActualSignedVelocityYawRate, DefaultFrame.SteeringExecution);
		TEST_FIELD_DEFAULT(Frame.SteeringExecution, bHasVelocityYawRate, DefaultFrame.SteeringExecution);
		Test.TestFalse(Label(TEXT("Motion.SingleSequenceTime cleared")), Frame.MotionExecution.bHasSingleSequenceTime);
		Test.TestNull(Label(TEXT("Motion.Sequence reference cleared")), Frame.MotionExecution.SingleSequence.Get());
		Test.TestEqual(Label(TEXT("Motion.SequenceEnd cleared")), Frame.MotionExecution.SequenceEndSeconds, 0.0);
		Test.TestFalse(Label(TEXT("Motion.Ended cleared")), Frame.MotionExecution.bEnded);
		Test.TestEqual(Label(TEXT("Steering.Status")), static_cast<uint8>(Frame.LocomotionSteering.Status),
			static_cast<uint8>(DefaultFrame.LocomotionSteering.Status));
		Test.TestEqual(Label(TEXT("Steering.SourceEpoch")), Frame.LocomotionSteering.SourceEpoch, uint64(0));
		Test.TestEqual(Label(TEXT("Steering.CompletedIntervalSerial")), Frame.LocomotionSteering.CompletedIntervalSerial, uint64(0));
		Test.TestEqual(Label(TEXT("Steering.ActualSignedVelocityYawRate")), Frame.LocomotionSteering.ActualSignedVelocityYawRate, 0.0f);
		Test.TestEqual(Label(TEXT("Steering.DesiredDirectionError")), Frame.LocomotionSteering.DesiredDirectionError, 0.0f);
		Test.TestFalse(Label(TEXT("Steering.bHasVelocityYawRate")), Frame.LocomotionSteering.bHasVelocityYawRate);
		Test.TestTrue(Label(TEXT("Steering.Diagnostic empty")), Frame.LocomotionSteering.Diagnostic.IsEmpty());
		Test.TestFalse(Label(TEXT("Steering.OriginalMovement cleared")), Frame.LocomotionSteering.OriginalMovement.IsValid());
		Test.TestFalse(Label(TEXT("Steering.OriginalCharacter cleared")), Frame.LocomotionSteering.OriginalCharacter.IsValid());
		Test.TestFalse(Label(TEXT("Steering.OriginalUpdatedComponent cleared")), Frame.LocomotionSteering.OriginalUpdatedComponent.IsValid());
		TEST_FIELD_DEFAULT(Frame, StopMotionType, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, Gait, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, MovementMode, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, TurnBackPhase, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, bHasMoveInput, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, bMovingHorizontally, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, bGrounded, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, bMovementBlocked, DefaultFrame);
		TEST_FIELD_DEFAULT(Frame, bTurnBackRunOut, DefaultFrame);
		Test.TestTrue(Label(TEXT("OwnedStateTags empty")), Frame.OwnedStateTags.IsEmpty());

		TEST_FIELD_DEFAULT(Debug, CurveVelocity, DefaultDebug);
		TEST_FIELD_DEFAULT(Debug, CurveVelocityDirection, DefaultDebug);
		TEST_FIELD_DEFAULT(Debug, CurveVelocityAngle, DefaultDebug);
		TEST_FIELD_DEFAULT(Debug, InputForwardDot, DefaultDebug);

		TEST_FIELD_DEFAULT(Snap, Gait, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, WalkRunBlendAlpha, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, StopMotionType, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, bShouldMove, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, AnimBlendX, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, AnimBlendY, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, AnimCurveVelocity, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, AnimCurveVelocityDirection, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, AnimCurveVelocityAngle, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, InputForwardDot, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, TurnBackPhase, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, bTurnBackRunOut, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, VelocityLength, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, ActualVelocityDirection, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, ActualVelocityBlendX, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, ActualVelocityBlendY, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, ActualVelocityAngle, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, bGrounded, DefaultSnap);
		TEST_FIELD_DEFAULT(Snap, bBlockMove, DefaultSnap);

		Test.TestEqual(Label(TEXT("StateMemory.GaitBlendY")), AnimInstance.StateMemory.GaitBlendY, DefaultMemory.GaitBlendY);
		Test.TestEqual(Label(TEXT("StateMemory.StopValue")), AnimInstance.StateMemory.StopValue, DefaultMemory.StopValue);
		Test.TestEqual(Label(TEXT("AnimBlendX")), AnimInstance.AnimBlendX, 0.0f);
		Test.TestEqual(Label(TEXT("AnimBlendY")), AnimInstance.AnimBlendY, 0.0f);
		Test.TestEqual(Label(TEXT("AnimCurveVelocity")), AnimInstance.AnimCurveVelocity, FVector::ZeroVector);
		Test.TestEqual(Label(TEXT("AnimCurveVelocityDirection")), AnimInstance.AnimCurveVelocityDirection, FVector::ZeroVector);
		Test.TestEqual(Label(TEXT("AnimCurveVelocityAngle")), AnimInstance.AnimCurveVelocityAngle, 0.0f);
		Test.TestEqual(Label(TEXT("ActualVelocityDirection")), AnimInstance.ActualVelocityDirection, FVector::ZeroVector);
		Test.TestEqual(Label(TEXT("ActualVelocityBlendX")), AnimInstance.ActualVelocityBlendX, 0.0f);
		Test.TestEqual(Label(TEXT("ActualVelocityBlendY")), AnimInstance.ActualVelocityBlendY, 0.0f);
		Test.TestEqual(Label(TEXT("ActualVelocityAngle")), AnimInstance.ActualVelocityAngle, 0.0f);
		Test.TestFalse(Label(TEXT("bTurnBackRunOut")), AnimInstance.bTurnBackRunOut);
		Test.TestEqual(Label(TEXT("WalkRunLeanAngleDegrees")), AnimInstance.WalkRunLeanAngleDegrees, 0.0f);
		Test.TestTrue(Label(TEXT("WalkRunLeanAxisCS")), AnimInstance.WalkRunLeanAxisCS.IsZero());
		Test.TestTrue(Label(TEXT("WalkRunLeanUpCS")), AnimInstance.WalkRunLeanUpCS.IsZero());
		Test.TestFalse(Label(TEXT("bWalkRunLeanPresentationValid")), AnimInstance.bWalkRunLeanPresentationValid);
		Test.TestFalse(Label(TEXT("bWalkRunLeanTargetApplicable")), AnimInstance.bWalkRunLeanTargetApplicable);
		Test.TestTrue(Label(TEXT("WalkRunLeanFailureReason empty")), AnimInstance.WalkRunLeanFailureReason.IsEmpty());

#undef TEST_FIELD_DEFAULT
	}

	void SetTestCharacterFrame(
		AGGYGOAnimationLifecycleTestCharacter& Character,
		const FVector& Velocity,
		const FVector& Acceleration,
		EGGYGOGait Gait,
		float WalkRunBlendAlpha,
		EGGYGOStopMotionType StopMotionType,
		EGGYGOTurnBackPhase TurnBackPhase,
		const FVector& CurveVelocity,
		const FVector& CurveDirection,
		float CurveDirectionAngle,
		FGameplayTag OwnedTag)
	{
		Character.SetActorRotation(FRotator::ZeroRotator);
		UGGYGOAnimationLifecycleTestMovementComponent* Movement =
			Cast<UGGYGOAnimationLifecycleTestMovementComponent>(Character.GetCharacterMovement());
		check(Movement);
		Movement->SetUpdatedComponent(Character.GetCapsuleComponent());
		Movement->MovementMode = MOVE_Walking;
		Movement->SetAnimationTestFrame(
			Velocity, Acceleration, Gait, WalkRunBlendAlpha, StopMotionType, TurnBackPhase,
			CurveVelocity, CurveDirection, CurveDirectionAngle);
		if (Character.GetAbilitySystemComponent() && OwnedTag.IsValid())
		{
			Character.GetAbilitySystemComponent()->AddLooseGameplayTag(OwnedTag);
		}
	}

	void TestOwnerFrame(
		FAutomationTestBase& Test,
		const UGGYGOAnimationLifecycleTestAnimInstance& AnimInstance,
		const TCHAR* Prefix,
		const FVector& ExpectedVelocity,
		float ExpectedWalkRunBlendAlpha,
		EGGYGOGait ExpectedGait,
		EGGYGOStopMotionType ExpectedStopMotion,
		int32 ExpectedStopValue,
		EGGYGOTurnBackPhase ExpectedTurnBackPhase,
		const FVector& ExpectedCurveVelocity,
		FGameplayTag ExpectedOwnedTag,
		FGameplayTag AbsentOwnedTag,
		float ExpectedLegacyBlendX,
		float ExpectedLegacyBlendY)
	{
		const FString PrefixString(Prefix);
		const auto Label = [&PrefixString](const TCHAR* Field)
		{
			return FString::Printf(TEXT("%s.%s"), *PrefixString, Field);
		};
		const FGGYGOAnimationStateFrame& Frame = AnimInstance.GetAnimationStateFrame();
		const FZZZAnimSnapshot& Snap = AnimInstance.GetSnapshotForTest();

		Test.TestEqual(Label(TEXT("base velocity")), Frame.WorldVelocity, ExpectedVelocity);
		Test.TestEqual(Label(TEXT("base gait")), Frame.Gait, ExpectedGait);
		Test.TestEqual(Label(TEXT("base stop semantic")), Frame.StopMotionType, ExpectedStopMotion);
		Test.TestEqual(Label(TEXT("base turnback phase")), Frame.TurnBackPhase, ExpectedTurnBackPhase);
		const float ExpectedHorizontalSpeed = static_cast<float>(ExpectedVelocity.Size2D());
		Test.TestEqual(Label(TEXT("base horizontal speed")), Frame.HorizontalSpeed, ExpectedHorizontalSpeed);
		Test.TestTrue(Label(TEXT("base move input")), Frame.bHasMoveInput);
		Test.TestTrue(Label(TEXT("base grounded")), Frame.bGrounded);
		Test.TestTrue(Label(TEXT("owner tag present")), AnimInstance.HasAnimationStateTag(ExpectedOwnedTag));
		Test.TestFalse(Label(TEXT("previous owner tag absent")), AnimInstance.HasAnimationStateTag(AbsentOwnedTag));

		Test.TestEqual(Label(TEXT("snapshot velocity")), Snap.ActualVelocityDirection, ExpectedVelocity.GetSafeNormal2D());
		Test.TestEqual(Label(TEXT("snapshot turnback phase")), Snap.TurnBackPhase, ExpectedTurnBackPhase);
		Test.TestEqual(Label(TEXT("snapshot curve velocity")), Snap.AnimCurveVelocity, ExpectedCurveVelocity);
		Test.TestEqual(Label(TEXT("snapshot legacy blend X")), Snap.AnimBlendX, ExpectedLegacyBlendX);
		Test.TestEqual(Label(TEXT("snapshot legacy blend Y")), Snap.AnimBlendY, ExpectedLegacyBlendY);
		Test.TestEqual(Label(TEXT("public legacy blend X")), AnimInstance.AnimBlendX, ExpectedLegacyBlendX);
		Test.TestEqual(Label(TEXT("public legacy blend Y")), AnimInstance.AnimBlendY, ExpectedLegacyBlendY);
		Test.TestEqual(Label(TEXT("public curve velocity")), AnimInstance.AnimCurveVelocity, ExpectedCurveVelocity);
		Test.TestEqual(Label(TEXT("public velocity direction")), AnimInstance.ActualVelocityDirection, ExpectedVelocity.GetSafeNormal2D());
		Test.TestEqual(Label(TEXT("public RunOut")), AnimInstance.bTurnBackRunOut, ExpectedTurnBackPhase == EGGYGOTurnBackPhase::RunOut);

		// D5 serialized AnimBP fields remain live compatibility outputs.
		Test.TestEqual(Label(TEXT("D5 GaitBlendY")), AnimInstance.StateMemory.GaitBlendY, ExpectedWalkRunBlendAlpha);
		Test.TestEqual(Label(TEXT("D5 StopValue")), AnimInstance.StateMemory.StopValue, ExpectedStopValue);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGGYGOAnimationLifecycleResetTest,
	"GGYGO.Animation.Lifecycle.ResetCallbacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAnimationLifecycleResetTest::RunTest(const FString& Parameters)
{
	// This test calls Native* directly. UAnimInstance::InitializeAnimation() requires an
	// initialized SkeletalMesh/proxy and is intentionally outside this test's boundary.
	TStrongObjectPtr<USkeletalMeshComponent> Mesh(
		NewObject<USkeletalMeshComponent>(GetTransientPackage()));
	if (!TestNotNull(TEXT("Transient SkeletalMeshComponent outer"), Mesh.Get()))
	{
		return false;
	}
	TStrongObjectPtr<UGGYGOAnimationLifecycleTestAnimInstance> AnimInstance(
		NewObject<UGGYGOAnimationLifecycleTestAnimInstance>(Mesh.Get()));
	if (!TestNotNull(TEXT("Transient lifecycle AnimInstance"), AnimInstance.Get()))
	{
		return false;
	}

	AnimInstance->SetTestPawnOwner(nullptr);
	AnimInstance->Tuning.WalkRunLean.FullLeanDirectionErrorDegrees = 180.0f;
	AnimInstance->Tuning.WalkRunLean.FullLeanYawRateDegreesPerSecond = 180.0f;
	AnimInstance->Tuning.WalkRunLean.WalkMaxAngleDegrees = 2.0f;
	AnimInstance->Tuning.WalkRunLean.RunMaxAngleDegrees = 6.0f;
	AnimInstance->Tuning.WalkRunLean.EnterResponseSpeed = 12.0f;
	AnimInstance->Tuning.WalkRunLean.RecoveryResponseSpeed = 8.0f;
	AnimInstance->InvokeNativeInitializeAnimation();
	TestClearedLifecycleState(*this, *AnimInstance, TEXT("empty Owner initialize"));

	AnimInstance->SeedDirtyRuntimeStateForTest();
	AnimInstance->InvokeNativeInitializeAnimation();
	TestClearedLifecycleState(*this, *AnimInstance, TEXT("repeated initialize"));

	AnimInstance->SeedDirtyRuntimeStateForTest();
	AnimInstance->InvokeNativeUninitializeAnimation();
	TestClearedLifecycleState(*this, *AnimInstance, TEXT("uninitialize"));

	AnimInstance->SeedDirtyRuntimeStateForTest();
	AnimInstance->InvokeNativeUninitializeAnimation();
	TestClearedLifecycleState(*this, *AnimInstance, TEXT("repeated uninitialize"));
	TestFalse(TEXT("reset preserves explicit disabled lean mode"), AnimInstance->Tuning.WalkRunLean.bEnabled);
	TestEqual(TEXT("reset preserves authored direction reference"), AnimInstance->Tuning.WalkRunLean.FullLeanDirectionErrorDegrees, 180.0f);
	TestEqual(TEXT("reset preserves serialized legacy rate without converting units"), AnimInstance->Tuning.WalkRunLean.FullLeanYawRateDegreesPerSecond, 180.0f);
	TestEqual(TEXT("reset preserves authored Run maximum"), AnimInstance->Tuning.WalkRunLean.RunMaxAngleDegrees, 6.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGGYGOAnimationLifecycleOwnerChangeTest,
	"GGYGO.Animation.Lifecycle.OwnerChangeAndD5Compatibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAnimationLifecycleOwnerChangeTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("Engine"), GEngine))
	{
		return false;
	}

	FScopedAnimationTestWorld TestWorld;
	if (!TestNotNull(TEXT("Transient world"), TestWorld.World))
	{
		return false;
	}

	AGGYGOAnimationLifecycleTestCharacter* OwnerA =
		TestWorld.World->SpawnActor<AGGYGOAnimationLifecycleTestCharacter>();
	AGGYGOAnimationLifecycleTestCharacter* OwnerB =
		TestWorld.World->SpawnActor<AGGYGOAnimationLifecycleTestCharacter>();
	if (!TestNotNull(TEXT("Owner A"), OwnerA) || !TestNotNull(TEXT("Owner B"), OwnerB))
	{
		return false;
	}

	const FGameplayTag TagA = GGYGOGameplayTags::State_Idle;
	const FGameplayTag TagB = GGYGOGameplayTags::State_Moving;
	SetTestCharacterFrame(
		*OwnerA, FVector(300.0f, 0.0f, 0.0f), FVector(1.0f, 0.0f, 0.0f),
		EGGYGOGait::Walk, 0.25f, EGGYGOStopMotionType::WalkStop,
		EGGYGOTurnBackPhase::Braking, FVector(120.0f, 0.0f, 0.0f), FVector(1.0f, 0.0f, 0.0f), 15.0f, TagA);
	SetTestCharacterFrame(
		*OwnerB, FVector(0.0f, 600.0f, 0.0f), FVector(0.0f, 1.0f, 0.0f),
		EGGYGOGait::Run, 0.85f, EGGYGOStopMotionType::RunStop,
		EGGYGOTurnBackPhase::RunOut, FVector(0.0f, 240.0f, 0.0f), FVector(0.0f, 1.0f, 0.0f), 90.0f, TagB);

	TStrongObjectPtr<USkeletalMeshComponent> Mesh(
		NewObject<USkeletalMeshComponent>(GetTransientPackage()));
	if (!TestNotNull(TEXT("Transient SkeletalMeshComponent outer"), Mesh.Get()))
	{
		return false;
	}
	TStrongObjectPtr<UGGYGOAnimationLifecycleTestAnimInstance> AnimInstance(
		NewObject<UGGYGOAnimationLifecycleTestAnimInstance>(Mesh.Get()));
	if (!TestNotNull(TEXT("Transient lifecycle AnimInstance"), AnimInstance.Get()))
	{
		return false;
	}

	AnimInstance->SetTestPawnOwner(OwnerA);
	AnimInstance->InvokeNativeInitializeAnimation();
	AnimInstance->InvokeNativeUpdateAnimation(1.0f / 60.0f);
	TestOwnerFrame(
		*this, *AnimInstance, TEXT("Owner A"), FVector(300.0f, 0.0f, 0.0f), 0.25f,
		EGGYGOGait::Walk, EGGYGOStopMotionType::WalkStop, 1, EGGYGOTurnBackPhase::Braking,
		FVector(120.0f, 0.0f, 0.0f), TagA, TagB, 0.0f, 1.0f);

	AnimInstance->SetTestPawnOwner(nullptr);
	AnimInstance->InvokeNativeUpdateAnimation(1.0f / 60.0f);
	TestClearedLifecycleState(*this, *AnimInstance, TEXT("Owner A to nullptr"));

	AnimInstance->SetTestPawnOwner(OwnerB);
	AnimInstance->InvokeNativeUpdateAnimation(1.0f / 60.0f);
	TestOwnerFrame(
		*this, *AnimInstance, TEXT("nullptr to Owner B"), FVector(0.0f, 600.0f, 0.0f), 0.85f,
		EGGYGOGait::Run, EGGYGOStopMotionType::RunStop, 2, EGGYGOTurnBackPhase::RunOut,
		FVector(0.0f, 240.0f, 0.0f), TagB, TagA, 1.0f, 0.0f);

	// Also cover a direct A -> B rebind on a single update frame.
	AnimInstance->SetTestPawnOwner(OwnerA);
	AnimInstance->InvokeNativeUpdateAnimation(1.0f / 60.0f);
	AnimInstance->SetTestPawnOwner(OwnerB);
	AnimInstance->InvokeNativeUpdateAnimation(1.0f / 60.0f);
	TestOwnerFrame(
		*this, *AnimInstance, TEXT("direct Owner A to Owner B"), FVector(0.0f, 600.0f, 0.0f), 0.85f,
		EGGYGOGait::Run, EGGYGOStopMotionType::RunStop, 2, EGGYGOTurnBackPhase::RunOut,
		FVector(0.0f, 240.0f, 0.0f), TagB, TagA, 1.0f, 0.0f);

	// After a previously valid weak owner expires, TryGetPawnOwner returns null while
	// the cached weak reference is also invalid. This exercises the explicit lost-owner path.
	AnimInstance->SetTestPawnOwner(OwnerA);
	AnimInstance->InvokeNativeUpdateAnimation(1.0f / 60.0f);
	AnimInstance->SeedDirtyRuntimeStateForTest();
	OwnerA->Destroy();
	AnimInstance->InvokeNativeUpdateAnimation(1.0f / 60.0f);
	TestClearedLifecycleState(*this, *AnimInstance, TEXT("expired weak Owner to nullptr"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGGYGOAnimationLocomotionEventsResetTest,
	"GGYGO.Animation.Lifecycle.LocomotionEventsReset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAnimationLocomotionEventsResetTest::RunTest(const FString& Parameters)
{
	FZZZAnimSnapshot OldSnapshot;
	OldSnapshot.WalkRunBlendAlpha = 0.75f;
	OldSnapshot.StopMotionType = EGGYGOStopMotionType::RunStop;
	FZZZAnimStateMemory Memory;
	FZZZAnimTuning Tuning;
	FZZZAnimWriteContext OldContext;
	OldContext.Snap = &OldSnapshot;
	OldContext.Memory = &Memory;
	OldContext.Tuning = &Tuning;

	FZZZLocomotionEvents Events;
	Events.SetContext(OldContext);
	Events.MapMovementState();
	TestEqual(TEXT("initial D5 GaitBlendY write"), Memory.GaitBlendY, 0.75f);
	TestEqual(TEXT("initial D5 StopValue write"), Memory.StopValue, 2);

	Events.Reset();
	OldSnapshot.WalkRunBlendAlpha = 0.2f;
	OldSnapshot.StopMotionType = EGGYGOStopMotionType::WalkStop;
	Events.MapMovementState();
	TestEqual(TEXT("reset detaches old Snapshot from Memory"), Memory.GaitBlendY, 0.75f);
	TestEqual(TEXT("reset prevents old Stop semantic from writing"), Memory.StopValue, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGGYGOAnimationWalkRunLeanTest,
	"GGYGO.Animation.WalkRunLean.PresentationAndReset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAnimationWalkRunLeanTest::RunTest(const FString& Parameters)
{
	// Captured consumer contract only. Real CMC qualification, bone axes and production
	// Action/WalkRun composition remain integration smoke gates, not proof from this fixture.
	FScopedAnimationTestWorld TestWorld;
	if (!TestNotNull(TEXT("World"), TestWorld.World))
	{
		return false;
	}
	AGGYGOAnimationLifecycleTestCharacter* SourceA = TestWorld.World->SpawnActor<AGGYGOAnimationLifecycleTestCharacter>();
	AGGYGOAnimationLifecycleTestCharacter* SourceB = TestWorld.World->SpawnActor<AGGYGOAnimationLifecycleTestCharacter>();
	if (!TestNotNull(TEXT("Source A"), SourceA) || !TestNotNull(TEXT("Source B"), SourceB))
	{
		return false;
	}
	TStrongObjectPtr<USkeletalMeshComponent> Mesh(NewObject<USkeletalMeshComponent>(GetTransientPackage()));
	TStrongObjectPtr<UGGYGOAnimationLifecycleTestAnimInstance> Anim(NewObject<UGGYGOAnimationLifecycleTestAnimInstance>(Mesh.Get()));
	FZZZWalkRunLeanTuning& Config = Anim->Tuning.WalkRunLean;
	Config.bEnabled = true;
	Config.FullLeanDirectionErrorDegrees = 180.0f;
	Config.WalkMaxAngleDegrees = 2.0f;
	Config.RunMaxAngleDegrees = 6.0f;
	Config.RecoveryResponseSpeed = 8.0f;
	// Keep a positive serialized legacy configuration to prove it cannot satisfy the new angle contract.
	Config.FullLeanYawRateDegreesPerSecond = 180.0f;
	Config.EnterResponseSpeed = 12.0f;
	FRichCurve* Response = Config.WalkDirectionErrorToLeanDegrees.GetRichCurve();
	FRichCurve* RunResponse = Config.RunDirectionErrorToLeanDegrees.GetRichCurve();
	// 单曲线仍有合法旧数据，不能代替任一缺失的新步态曲线。
	Config.DirectionErrorToLeanDegrees.GetRichCurve()->AddKey(0.0f, 0.0f);
	Config.DirectionErrorToLeanDegrees.GetRichCurve()->AddKey(180.0f, 80.0f);
	const auto ConfigureResponse = [Response, RunResponse]()
	{
		for (FRichCurve* Curve : {Response, RunResponse})
		{
			Curve->Reset();
			const float Scale = Curve == Response ? 0.5f : 1.0f;
			for (const FVector2D Point : { FVector2D(0.0, 0.0), FVector2D(20.0, 20.0), FVector2D(60.0, 45.0), FVector2D(180.0, 75.0) })
			{
				const FKeyHandle Key = Curve->AddKey(static_cast<float>(Point.X), static_cast<float>(Point.Y) * Scale);
				Curve->SetKeyInterpMode(Key, RCIM_Linear);
			}
		}
	};
	ConfigureResponse();

	FGGYGOAnimationStateFrame Frame;
	Frame.Gait = EGGYGOGait::Walk;
	Frame.bLocomotionSteeringCaptured = true;
	Frame.bCapsuleMeshBasisValid = true;
	Frame.LeanAxisCS = -FVector::ForwardVector;
	Frame.CapsuleUpCS = FVector::UpVector;
	FGGYGOLocomotionSteeringSnapshot& Steering = Frame.LocomotionSteering;
	Steering.OriginalMovement = Cast<UGGYGOCharacterMovementComponent>(SourceA->GetCharacterMovement());
	Steering.OriginalCharacter = SourceA;
	Steering.OriginalUpdatedComponent = SourceA->GetCapsuleComponent();
	Steering.SourceEpoch = 1;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestFalse(TEXT("Initial is not an observed zero direction deviation"), Anim->bWalkRunLeanPresentationValid);
	TestFalse(TEXT("Initial没有方向目标可供图消费"), Anim->bWalkRunLeanTargetApplicable);
	TestTrue(TEXT("Initial is a normal wait"), Anim->WalkRunLeanFailureReason.IsEmpty());

	Steering.Status = EGGYGOLocomotionSteeringStatus::Valid;
	Steering.CameraTurnStatus = EGGYGOLocomotionSteeringStatus::Valid;
	Steering.CameraTurnGait = EGGYGOGait::Walk;
	Steering.CameraTurnWalkRunBlendAlpha = 0.0f;
	Steering.DesiredDirectionError = -90.0f;
	Steering.CompletedIntervalSerial = 1;
	Steering.NativeDeltaSeconds = 1.0f / 60.0f;
	Steering.bHasVelocityYawRate = true;
	Steering.ActualSignedVelocityYawRate = -180.0f;
	Steering.ActualSignedYawRate = -180.0f;
	Steering.CameraTurnDemandDegrees = 30.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	const float Walk30 = Anim->WalkRunLeanAngleDegrees;
	TestTrue(TEXT("right direction deviation wins over opposite velocity and capsule yaw rates"), Walk30 > 0.0f);
	TestEqual(TEXT("原生只发布本次角色Walk响应目标"), Walk30, 13.125f);
	Steering.CameraTurnDemandDegrees = 60.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	const float Walk60 = Anim->WalkRunLeanAngleDegrees;
	TestEqual(TEXT("方向60度映射明确配置的Walk倾身"), Walk60, 22.5f);
	Steering.CameraTurnDemandDegrees = -60.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestEqual(TEXT("left direction is the signed mirror without entry lag"), Anim->WalkRunLeanAngleDegrees, -Walk60);
	Steering.CameraTurnDemandDegrees = 0.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestEqual(TEXT("真实零偏差直接发布零目标"), Anim->WalkRunLeanAngleDegrees, 0.0f);
	TestTrue(TEXT("真实零目标保留输入健康资格"), Anim->bWalkRunLeanPresentationValid);
	TestTrue(TEXT("真实零目标与不适用分开发布"), Anim->bWalkRunLeanTargetApplicable);

	Frame.WalkRunBlendAlpha = 1.0f;
	Frame.Gait = EGGYGOGait::Run;
	Steering.CameraTurnGait = EGGYGOGait::Run;
	Steering.CameraTurnWalkRunBlendAlpha = 1.0f;
	Steering.CameraTurnDemandDegrees = 30.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	const float Run30 = Anim->WalkRunLeanAngleDegrees;
	TestEqual(TEXT("Run采用独立角色曲线端点，不缩放旧Run最大值"), Run30, 26.25f);
	Steering.CameraTurnDemandDegrees = 60.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestEqual(TEXT("Run采用角色明确配置的60度响应"), Anim->WalkRunLeanAngleDegrees, 45.0f);
	Steering.CameraTurnDemandDegrees = 30.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestEqual(TEXT("本次误差变化直接采用本次响应"), Anim->WalkRunLeanAngleDegrees, Run30);
	Steering.bHasVelocityYawRate = false;
	Steering.ActualSignedVelocityYawRate = 0.0f;
	Steering.ActualSignedYawRate = 720.0f;
	Steering.NativeDeltaSeconds = 1.0f / 30.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestEqual(TEXT("方向目标不依赖速度导数或native区间长度"), Anim->WalkRunLeanAngleDegrees, Run30);
	Steering.CameraTurnDemandDegrees = 90.0f;
	Frame.WalkRunBlendAlpha = 0.5f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestEqual(TEXT("alpha中途仍只发布Run端点，不在原生再做一次步态混合"), Anim->WalkRunLeanAngleDegrees, 52.5f);
	TestEqual(TEXT("表现不改变Movement步态alpha"), Frame.WalkRunBlendAlpha, 0.5f);
	Steering.CameraTurnDemandDegrees = 180.0f;
	Frame.WalkRunBlendAlpha = 0.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestEqual(TEXT("完整方向域覆盖明确曲线终点"), Anim->WalkRunLeanAngleDegrees, 75.0f);
	Frame.WalkRunBlendAlpha = 1.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestEqual(TEXT("旧Walk/Run最大值不竞争新响应"), Anim->WalkRunLeanAngleDegrees, 75.0f);
	Steering.CameraTurnDemandDegrees = -180.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestEqual(TEXT("完整左偏差采用曲线的负向镜像"), Anim->WalkRunLeanAngleDegrees, -75.0f);

	Steering.CameraTurnDemandDegrees = 90.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	Steering.Status = EGGYGOLocomotionSteeringStatus::NotApplicable;
	Steering.CameraTurnStatus = EGGYGOLocomotionSteeringStatus::NotApplicable;
	Steering.CameraTurnDemandDegrees = 180.0f;
	Frame.bHasMoveInput = true;
	Frame.Gait = EGGYGOGait::Run;
	Frame.TurnBackPhase = EGGYGOTurnBackPhase::Turning;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestFalse(TEXT("健康NotApplicable不能伪装有效方向零观测"), Anim->bWalkRunLeanTargetApplicable);
	TestEqual(TEXT("不适用时原生不保留旧显示角或执行恢复"), Anim->WalkRunLeanAngleDegrees, 0.0f);
	TestTrue(TEXT("健康门不提前关闭图内TurnBack有限回正"), Anim->bWalkRunLeanPresentationValid);
	for (int32 Repeat = 0; Repeat < 3; ++Repeat) Anim->InvokeWalkRunLeanForTest(Frame);
	TestEqual(TEXT("重复不适用不累计原生表现状态"), Anim->WalkRunLeanAngleDegrees, 0.0f);
	TestTrue(TEXT("精确零不会退休真实依赖健康门"), Anim->bWalkRunLeanPresentationValid);

	Steering.Status = EGGYGOLocomotionSteeringStatus::Valid;
	Steering.CameraTurnStatus = EGGYGOLocomotionSteeringStatus::Valid;
	Steering.CameraTurnDemandDegrees = 90.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	++Steering.SourceEpoch;
	Steering.CameraTurnDemandDegrees = 0.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestEqual(TEXT("new valid epoch uses its own zero direction, not the old pose"), Anim->WalkRunLeanAngleDegrees, 0.0f);
	TestTrue(TEXT("new epoch valid zero observation remains usable"), Anim->bWalkRunLeanPresentationValid);
	Steering.CameraTurnDemandDegrees = -90.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	++Steering.SourceEpoch;
	Steering.Status = EGGYGOLocomotionSteeringStatus::NotApplicable;
	Steering.CameraTurnStatus = EGGYGOLocomotionSteeringStatus::NotApplicable;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestEqual(TEXT("新epoch目标不继承旧显示角"), Anim->WalkRunLeanAngleDegrees, 0.0f);
	TestTrue(TEXT("新epoch的健康不适用仍保依赖资格"), Anim->bWalkRunLeanPresentationValid);
	TestFalse(TEXT("新epoch不将旧响应声明为适用目标"), Anim->bWalkRunLeanTargetApplicable);
	Steering.Status = EGGYGOLocomotionSteeringStatus::Valid;
	Steering.CameraTurnStatus = EGGYGOLocomotionSteeringStatus::Valid;
	Steering.CameraTurnDemandDegrees = 90.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	Steering.OriginalMovement = Cast<UGGYGOCharacterMovementComponent>(SourceB->GetCharacterMovement());
	Steering.OriginalCharacter = SourceB;
	Steering.OriginalUpdatedComponent = SourceB->GetCapsuleComponent();
	Steering.Status = EGGYGOLocomotionSteeringStatus::NotApplicable;
	Steering.CameraTurnStatus = EGGYGOLocomotionSteeringStatus::NotApplicable;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestEqual(TEXT("不同CMC即使epoch相同也不继承上一响应目标"), Anim->WalkRunLeanAngleDegrees, 0.0f);
	TestTrue(TEXT("新CMC健康资格独立于上一目标角"), Anim->bWalkRunLeanPresentationValid);
	TestFalse(TEXT("不同CMC不继承上一适用目标"), Anim->bWalkRunLeanTargetApplicable);
	Steering.Status = EGGYGOLocomotionSteeringStatus::Valid;
	Steering.CameraTurnStatus = EGGYGOLocomotionSteeringStatus::Valid;
	Steering.CameraTurnDemandDegrees = -45.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestEqual(TEXT("新CMC只消费自身左偏差响应"), Anim->WalkRunLeanAngleDegrees, -35.625f);

	AddExpectedError(TEXT("[Animation][WalkRunLean]"), EAutomationExpectedErrorFlags::Contains, 5);
	Steering.Status = EGGYGOLocomotionSteeringStatus::Invalid;
	Steering.Diagnostic = TEXT("original movement provider retired");
	Anim->InvokeWalkRunLeanForTest(Frame);
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestEqual(TEXT("Invalid retires the original pose value"), Anim->WalkRunLeanAngleDegrees, 0.0f);
	TestFalse(TEXT("Invalid is failure rather than a usable neutral pose"), Anim->bWalkRunLeanPresentationValid);
	TestFalse(TEXT("Invalid同时拒绝方向目标消费"), Anim->bWalkRunLeanTargetApplicable);
	TestTrue(TEXT("original provider diagnostic remains visible"), Anim->WalkRunLeanFailureReason.Contains(TEXT("retired")));
	Config.FullLeanDirectionErrorDegrees = 0.0f;
	Response->Reset();
	Steering.Status = EGGYGOLocomotionSteeringStatus::Valid;
	Anim->InvokeWalkRunLeanForTest(Frame);
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestTrue(TEXT("仅旧字段不能填补缺失响应曲线"), Anim->WalkRunLeanFailureReason.Contains(TEXT("DirectionErrorToLeanDegrees")));
	TestEqual(TEXT("legacy yaw rate never fills the missing angle"), Config.FullLeanDirectionErrorDegrees, 0.0f);
	TestEqual(TEXT("legacy serialized units remain unchanged"), Config.FullLeanYawRateDegreesPerSecond, 180.0f);
	Config.FullLeanDirectionErrorDegrees = 90.0f;
	Response->AddKey(0.0f, 0.0f);
	Response->AddKey(60.0f, 45.0f);
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestTrue(TEXT("没有覆盖完整输入域的响应明确拒绝"), Anim->WalkRunLeanFailureReason.Contains(TEXT("[0,180]")));
	Config.FullLeanDirectionErrorDegrees = 180.0f;
	ConfigureResponse();
	Steering.CameraTurnDemandDegrees = std::numeric_limits<float>::quiet_NaN();
	Anim->InvokeWalkRunLeanForTest(Frame);
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestTrue(TEXT("nonfinite direction fails explicitly without per-frame diagnostic reset"), Anim->WalkRunLeanFailureReason.Contains(TEXT("CameraTurnDemandDegrees")));
	TestFalse(TEXT("illegal direction cannot publish a usable zero pose"), Anim->bWalkRunLeanPresentationValid);
	Steering.CameraTurnDemandDegrees = 181.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestTrue(TEXT("noncanonical captured angle is rejected rather than normalized or clamped"), Anim->WalkRunLeanFailureReason.Contains(TEXT("[-180,180]")));
	Steering.CameraTurnDemandDegrees = 20.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestEqual(TEXT("方向20度精确采用角色响应"), Anim->WalkRunLeanAngleDegrees, 20.0f);
	TestTrue(TEXT("输出倾身轴与GT捕获一致"), Anim->WalkRunLeanAxisCS.Equals(Frame.LeanAxisCS, 0.0));
	Frame.bCapsuleMeshBasisValid = false;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestFalse(TEXT("无真实胶囊/Mesh基准不生成有效姿态驱动"), Anim->bWalkRunLeanPresentationValid);
	Frame.bCapsuleMeshBasisValid = true;
	Anim->InvokeWalkRunLeanForTest(Frame);
	const FKeyHandle OvershootKey = RunResponse->FindKey(20.0f);
	RunResponse->SetKeyInterpMode(OvershootKey, RCIM_Cubic);
	RunResponse->GetKey(OvershootKey).TangentMode = RCTM_User;
	RunResponse->GetKey(OvershootKey).LeaveTangent = 100.0f;
	Steering.CameraTurnDemandDegrees = 30.0f;
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestTrue(TEXT("有限控制点的cubic超限求值也明确拒绝"), Anim->WalkRunLeanFailureReason.Contains(TEXT("[0,90)")));
	TestFalse(TEXT("超限响应不被夹成成功角度"), Anim->bWalkRunLeanPresentationValid);
	ConfigureResponse();
	Anim->InvokeNativeUninitializeAnimation();
	TestClearedLifecycleState(*this, *Anim, TEXT("lean lifecycle uninitialize"));
	TestTrue(TEXT("lifecycle preserves authored enable mode"), Config.bEnabled);
	TestEqual(TEXT("lifecycle preserves the authored direction reference"), Config.FullLeanDirectionErrorDegrees, 180.0f);
	TestTrue(TEXT("生命周期不清角色Walk/Run响应曲线"), Response->Eval(60.0f) == 22.5f && RunResponse->Eval(60.0f) == 45.0f);

	Config.bEnabled = false;
	Anim->SeedDirtyRuntimeStateForTest();
	Anim->InvokeWalkRunLeanForTest(Frame);
	TestFalse(TEXT("explicit disabled mode stops only lean presentation"), Anim->bWalkRunLeanPresentationValid);
	TestTrue(TEXT("disabled mode has no failure diagnostic"), Anim->WalkRunLeanFailureReason.IsEmpty());
	TestEqual(TEXT("lean disable does not clear another presentation resource"), Anim->StateMemory.GaitBlendY, 0.73f);
	Anim->InvokeNativeUninitializeAnimation();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAnimationMotionPoseConsumerTest,
	"GGYGO.Animation.MotionPose.Consumer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAnimationMotionPoseConsumerTest::RunTest(const FString& Parameters)
{
	using FView = FGGYGOAnimationMotionPoseView;
	constexpr auto Kind = EGGYGOLocomotionMotionType::TurnBack;
	FScopedAnimationTestWorld TestWorld;
	if (!TestNotNull(TEXT("消费端测试世界"), TestWorld.World)) return false;
	auto* SourceA = TestWorld.World->SpawnActor<AGGYGOAnimationLifecycleTestCharacter>();
	auto* SourceB = TestWorld.World->SpawnActor<AGGYGOAnimationLifecycleTestCharacter>();
	if (!TestNotNull(TEXT("原Provider"), SourceA) || !TestNotNull(TEXT("后继Provider"), SourceB)) return false;
	TStrongObjectPtr<USkeletalMeshComponent> Mesh(NewObject<USkeletalMeshComponent>(GetTransientPackage()));
	TStrongObjectPtr<UGGYGOAnimationLifecycleTestAnimInstance> Anim(NewObject<UGGYGOAnimationLifecycleTestAnimInstance>(Mesh.Get()));
	TStrongObjectPtr<UAnimSequence> Asset(NewObject<UAnimSequence>(GetTransientPackage()));
	Asset->RateScale = 2.0f;
	Anim->SetTestPawnOwner(SourceA);
	Anim->InvokeNativeInitializeAnimation();
	// 真实生命周期捕获 Provider；下述值只模拟认证后的消费输入，不证明 CMC、节点求值或网络。
	FGGYGOAnimationStateFrame Frame;
	FGGYGOAnimationMotionFrame& Motion = Frame.MotionExecution;
	Motion.Status = EGGYGOMotionExecutionStatus::Valid;
	Motion.SourceKind = EGGYGOMotionExecutionSource::Locomotion;
	Motion.MotionType = Kind;
	Motion.TurnBackPhase = EGGYGOTurnBackPhase::RunOut;
	Motion.SingleSequence = Asset.Get();
	Motion.bHasSingleSequenceTime = true;
	Motion.SequenceEndSeconds = 1.25;
	Motion.ExecutionRequestSerial = (uint64 { 1 } << 63) + 37;
	Motion.MotionSequence = 9;
	Motion.CompletedIntervalSerial = 1;
	const auto Query = [&]() { return Anim->QueryMotionPose(Kind); };
	Anim->SetCapturedMotionPoseForTest(Frame);
	UAnimSequence* Sequence = nullptr;
	float Time = 0.0f;
	bool bEnded = false;
	TestTrue(TEXT("RunOut仍可消费原完成姿态区间"), Anim->TryGetMotionPose(Kind, Sequence, Time, bEnded));
	TestEqual(TEXT("原准确Sequence时间不重复乘RateScale"), Time, 1.25f);
	TestTrue(TEXT("只返回原捕获的同源资产"), Sequence == Asset.Get());
	TestTrue(TEXT("RunOut相位不等于动作源结束"), Anim->IsMotionPoseActive(Kind));
	const FView Local = Query();
	TestTrue(TEXT("一次查询提供宿主、姿态和入场资格"), Local.bContextAvailable && Local.bHasPose && Local.bCanEnter);
	TestEqual(TEXT("新查询复用准确同源秒"), Local.SequenceSeconds, Time);
	TestTrue(TEXT("新查询复用同源资产与相位"), Local.Sequence == Asset.Get() && Local.TurnBackPhase == EGGYGOTurnBackPhase::RunOut);
	FView Copied;
	FView::StaticStruct()->CopyScriptStruct(&Copied, &Local);
	TestTrue(TEXT("反射结构复制保留opaque原始完整身份"), Anim->AreMotionPoseSourcesEqual(Local, Copied));
	++Motion.CompletedIntervalSerial;
	Motion.SequenceEndSeconds = 0.75;
	Anim->SetCapturedMotionPoseForTest(Frame);
	TestTrue(TEXT("区间序号和时间回跳不冒充新的片段身份"), Anim->AreMotionPoseSourcesEqual(Local, Query()));
	++Motion.MotionSequence;
	Anim->SetCapturedMotionPoseForTest(Frame);
	TestFalse(TEXT("同状态内新片段可被可靠识别"), Anim->AreMotionPoseSourcesEqual(Local, Query()));
	TestTrue(TEXT("更换片段不更换宿主上下文"), Anim->AreMotionPoseContextsEqual(Local, Query()));
	Motion.MotionSequence = 9;
	Motion.ExecutionRequestSerial += uint64 { 1 } << 32;
	Anim->SetCapturedMotionPoseForTest(Frame);
	TestFalse(TEXT("低32位相同的不同uint64请求不会合并"), Anim->AreMotionPoseSourcesEqual(Local, Query()));
	Motion.ExecutionRequestSerial = (uint64 { 1 } << 63) + 37;
	Motion.bEnded = true;
	Motion.EndReason = EGGYGOMotionExecutionEndReason::Completed;
	Anim->SetCapturedMotionPoseForTest(Frame);
	TestTrue(TEXT("末段终点仍可供原生退出混合取样"), Anim->TryGetMotionPose(Kind, Sequence, Time, bEnded));
	TestTrue(TEXT("终点明确发布Ended"), bEnded);
	TestFalse(TEXT("终点不冒称继续执行"), Anim->IsMotionPoseActive(Kind));
	TestTrue(TEXT("认证终点保留原来源供关闭混合"), Query().bEnded && Query().bHasPose && !Query().bCanEnter
		&& Anim->AreMotionPoseSourcesEqual(Local, Query()));
	Motion.Status = EGGYGOMotionExecutionStatus::NotApplicable;
	Motion.EndReason = EGGYGOMotionExecutionEndReason::Cancelled;
	Anim->SetCapturedMotionPoseForTest(Frame);
	TestFalse(TEXT("取消/失效事实不能设置播放器时间"), Anim->TryGetMotionPose(Kind, Sequence, Time, bEnded));
	TestNull(TEXT("失败查询不遗留资产参数"), Sequence);
	const FView Cancelled = Query();
	TestFalse(TEXT("取消不保旧片段键也不授予姿态"), Cancelled.bHasPose || Anim->AreMotionPoseSourcesEqual(Local, Cancelled));
	TestFalse(TEXT("两份空片段键不相等"), Anim->AreMotionPoseSourcesEqual(Cancelled, Cancelled));
	Motion.Status = EGGYGOMotionExecutionStatus::Invalid;
	Anim->SetCapturedMotionPoseForTest(Frame);
	TestFalse(TEXT("Invalid不能续用旧入场角来源"), Anim->AreMotionPoseSourcesEqual(Local, Query()));
	Motion.Status = EGGYGOMotionExecutionStatus::Valid;
	Motion.SourceKind = EGGYGOMotionExecutionSource::Montage;
	Anim->SetCapturedMotionPoseForTest(Frame);
	TestFalse(TEXT("复合Montage不能消费为单Sequence时钟"), Anim->TryGetMotionPose(Kind, Sequence, Time, bEnded));
	Anim->SetCapturedMotionPoseForTest({});
	TestTrue(TEXT("没有TurnBack仍可比较宿主，不每帧清普通gait记忆"), Query().bContextAvailable
		&& !Query().bHasPose && Anim->AreMotionPoseContextsEqual(Local, Query()));

	FGGYGOAnimationRemotePoseFrame Remote;
	Remote.bRemote = true;
	Remote.Status = static_cast<uint8>(EGGYGORemoteMotionPoseStatus::Current);
	Remote.bHasPose = true;
	Remote.bAdmissible = true;
	Remote.MotionType = Kind;
	Remote.TurnBackPhase = EGGYGOTurnBackPhase::Turning;
	Remote.Sequence = Asset.Get();
	Remote.SequenceSeconds = 0.4;
	Remote.PublisherEpoch = MAX_uint32 - 1;
	Remote.SegmentSerial = 7;
	Remote.PublicationSerial = 1;
	Anim->SetCapturedMotionPoseForTest({}, Remote);
	const FView Current = Query();
	TestTrue(TEXT("Current一次查询提供认证姿态及入场资格"), Current.bHasPose && Current.bCanEnter);
	TestEqual(TEXT("远端使用本条记录的原源秒"), Current.SequenceSeconds, 0.4f);
	TestFalse(TEXT("本机和远端身份不串用"), Anim->AreMotionPoseSourcesEqual(Local, Current));
	++Remote.PublicationSerial;
	Remote.PublicationToken = 123;
	Remote.SequenceSeconds = 0.45;
	Anim->SetCapturedMotionPoseForTest({}, Remote);
	TestTrue(TEXT("逐帧发布号不是来源身份"), Anim->AreMotionPoseSourcesEqual(Current, Query()));
	Remote.Status = static_cast<uint8>(EGGYGORemoteMotionPoseStatus::Held);
	Remote.bAdmissible = false;
	Anim->SetCapturedMotionPoseForTest({}, Remote);
	TestTrue(TEXT("合法Held只续同源姿态，不新锁存入场角"), Query().bHasPose && !Query().bCanEnter
		&& Anim->AreMotionPoseSourcesEqual(Current, Query()));
	Remote.bHasPose = false;
	Anim->SetCapturedMotionPoseForTest({}, Remote);
	TestFalse(TEXT("无进入资格的Held不产生片段键"), Anim->AreMotionPoseSourcesEqual(Current, Query()));
	Remote.Status = static_cast<uint8>(EGGYGORemoteMotionPoseStatus::Awaiting);
	Remote.bRetainedWait = true;
	Remote.Sequence = nullptr;
	Remote.SequenceSeconds = 0.0;
	Remote.TurnBackPhase = EGGYGOTurnBackPhase::Braking;
	Anim->SetCapturedMotionPoseForTest({}, Remote);
	const FView Waiting = Query();
	TestTrue(TEXT("保留等待仍携旧认证来源键"), Waiting.bRetainedWait && Anim->AreMotionPoseSourcesEqual(Current, Waiting));
	TestTrue(TEXT("保留等待无可采样时间或新入场资格"), !Waiting.bHasPose && !Waiting.bCanEnter && !Waiting.Sequence);
	TestTrue(TEXT("等待相位仍来自同一保留记录"), Waiting.TurnBackPhase == EGGYGOTurnBackPhase::Braking);
	++Remote.SegmentSerial;
	Anim->SetCapturedMotionPoseForTest({}, Remote);
	TestFalse(TEXT("不同片段的等待不能保旧入场角"), Anim->AreMotionPoseSourcesEqual(Current, Query()));
	Remote.SegmentSerial = 7;
	++Remote.PublisherEpoch;
	Anim->SetCapturedMotionPoseForTest({}, Remote);
	TestFalse(TEXT("不同发布端Epoch不能继承旧记忆"), Anim->AreMotionPoseSourcesEqual(Current, Query()));
	Remote.bRetainedWait = false;
	Remote.Status = static_cast<uint8>(EGGYGORemoteMotionPoseStatus::Invalid);
	Anim->SetCapturedMotionPoseForTest({}, Remote);
	TestFalse(TEXT("远端Invalid不产生默认来源键"), Anim->AreMotionPoseSourcesEqual(Query(), Query()));

	Remote.Status = static_cast<uint8>(EGGYGORemoteMotionPoseStatus::Current);
	Remote.bHasPose = Remote.bAdmissible = true;
	Remote.Sequence = Asset.Get();
	Remote.SequenceSeconds = 0.4;
	Anim->SetCapturedMotionPoseForTest({}, Remote);
	const FView OriginalHost = Query();
	Anim->SetTestPawnOwner(SourceB);
	Anim->InvokeNativeUpdateAnimation(0.0f);
	Anim->SetCapturedMotionPoseForTest({}, Remote);
	const FView NewHost = Query();
	TestFalse(TEXT("原Provider改变即退休所有旧宿主表现记忆"), Anim->AreMotionPoseContextsEqual(OriginalHost, NewHost));
	TestFalse(TEXT("相同Epoch/片段不能跨原Provider串用"), Anim->AreMotionPoseSourcesEqual(OriginalHost, NewHost));
	Anim->InvokeNativeUninitializeAnimation();
	TestFalse(TEXT("原生命周期结束清理运动姿态事实"), Anim->IsMotionPoseActive(Kind));
	TestFalse(TEXT("退休上下文为空且与自身也不相等"), Query().bContextAvailable || Anim->AreMotionPoseContextsEqual(Query(), Query()));
	Anim->InvokeNativeInitializeAnimation();
	Anim->SetCapturedMotionPoseForTest({}, Remote);
	TestFalse(TEXT("同一实例重初始化不继承原生命周期显示角"), Anim->AreMotionPoseContextsEqual(NewHost, Query()));
	TestFalse(TEXT("同Provider同片段也不能跨实例生命周期续用"), Anim->AreMotionPoseSourcesEqual(NewHost, Query()));
	Anim->InvokeNativeUninitializeAnimation();
	return !HasAnyErrors();
}

#endif

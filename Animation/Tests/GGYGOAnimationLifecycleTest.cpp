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
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

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
	AnimationState.LocomotionSteering.bHasVelocityYawRate = true;
	AnimationState.LocomotionSteering.Diagnostic = TEXT("previous captured interval");
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
	bWalkRunLeanPresentationValid = true;
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
		Test.TestEqual(Label(TEXT("Steering.Status")), static_cast<uint8>(Frame.LocomotionSteering.Status),
			static_cast<uint8>(DefaultFrame.LocomotionSteering.Status));
		Test.TestEqual(Label(TEXT("Steering.SourceEpoch")), Frame.LocomotionSteering.SourceEpoch, uint64(0));
		Test.TestEqual(Label(TEXT("Steering.CompletedIntervalSerial")), Frame.LocomotionSteering.CompletedIntervalSerial, uint64(0));
		Test.TestEqual(Label(TEXT("Steering.ActualSignedVelocityYawRate")), Frame.LocomotionSteering.ActualSignedVelocityYawRate, 0.0f);
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
		Test.TestFalse(Label(TEXT("bWalkRunLeanPresentationValid")), AnimInstance.bWalkRunLeanPresentationValid);
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
	TestEqual(TEXT("reset preserves authored full lean rate"), AnimInstance->Tuning.WalkRunLean.FullLeanYawRateDegreesPerSecond, 180.0f);
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
	// Deliberately test the captured consumer contract. Real CMC qualification and the
	// production WalkRun graph remain integration smoke gates, not proof from this fixture.
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
	Config.FullLeanYawRateDegreesPerSecond = 180.0f;
	Config.WalkMaxAngleDegrees = 2.0f;
	Config.RunMaxAngleDegrees = 6.0f;
	Config.EnterResponseSpeed = 12.0f;
	Config.RecoveryResponseSpeed = 8.0f;

	FGGYGOAnimationStateFrame Frame;
	Frame.bLocomotionSteeringCaptured = true;
	FGGYGOLocomotionSteeringSnapshot& Steering = Frame.LocomotionSteering;
	Steering.OriginalMovement = Cast<UGGYGOCharacterMovementComponent>(SourceA->GetCharacterMovement());
	Steering.OriginalCharacter = SourceA;
	Steering.OriginalUpdatedComponent = SourceA->GetCapsuleComponent();
	Steering.SourceEpoch = 1;
	Anim->InvokeWalkRunLeanForTest(Frame, 0.25f);
	TestFalse(TEXT("Initial does not manufacture an observed zero rate"), Anim->bWalkRunLeanPresentationValid);
	TestTrue(TEXT("Initial is a normal wait"), Anim->WalkRunLeanFailureReason.IsEmpty());

	Steering.Status = EGGYGOLocomotionSteeringStatus::Valid;
	Steering.CompletedIntervalSerial = 1;
	Steering.NativeDeltaSeconds = 1.0f / 60.0f;
	Steering.bHasVelocityYawRate = true;
	Steering.ActualSignedVelocityYawRate = 180.0f;
	Steering.ActualSignedYawRate = -180.0f;
	Anim->InvokeWalkRunLeanForTest(Frame, 0.25f);
	const float WalkAngle = Anim->WalkRunLeanAngleDegrees;
	TestTrue(TEXT("right trajectory wins over opposite capsule correction"), WalkAngle > 0.0f);
	TestTrue(TEXT("Walk stays within its authored limit"), WalkAngle <= Config.WalkMaxAngleDegrees);
	Anim->InvokeNativeUninitializeAnimation();
	Frame.WalkRunBlendAlpha = 1.0f;
	Anim->InvokeWalkRunLeanForTest(Frame, 0.25f);
	const float RunAngle = Anim->WalkRunLeanAngleDegrees;
	TestTrue(TEXT("same turn has stronger Run presentation"), RunAngle > WalkAngle && RunAngle <= Config.RunMaxAngleDegrees);

	Steering.bHasVelocityYawRate = false;
	Anim->InvokeWalkRunLeanForTest(Frame, 0.1f);
	TestTrue(TEXT("missing trajectory observation recovers without capsule substitution"),
		Anim->WalkRunLeanAngleDegrees > 0.0f && Anim->WalkRunLeanAngleDegrees < RunAngle);
	const float BeforeNotApplicable = Anim->WalkRunLeanAngleDegrees;
	Steering.Status = EGGYGOLocomotionSteeringStatus::NotApplicable;
	Steering.bHasVelocityYawRate = true;
	Frame.bHasMoveInput = true;
	Frame.Gait = EGGYGOGait::Run;
	Anim->InvokeWalkRunLeanForTest(Frame, 0.1f);
	TestTrue(TEXT("NotApplicable overrides otherwise moving Run frame"),
		Anim->WalkRunLeanAngleDegrees < BeforeNotApplicable);

	++Steering.SourceEpoch;
	Steering.Status = EGGYGOLocomotionSteeringStatus::Valid;
	Anim->InvokeWalkRunLeanForTest(Frame, 0.0f);
	TestEqual(TEXT("epoch change retires prior lean immediately"), Anim->WalkRunLeanAngleDegrees, 0.0f);
	TestTrue(TEXT("new epoch uses only its own completed observation"), Anim->bWalkRunLeanPresentationValid);
	Steering.ActualSignedVelocityYawRate = -180.0f;
	Anim->InvokeWalkRunLeanForTest(Frame, 0.25f);
	TestTrue(TEXT("left trajectory produces left lean"), Anim->WalkRunLeanAngleDegrees < 0.0f);
	Steering.OriginalMovement = Cast<UGGYGOCharacterMovementComponent>(SourceB->GetCharacterMovement());
	Steering.OriginalCharacter = SourceB;
	Steering.OriginalUpdatedComponent = SourceB->GetCapsuleComponent();
	Anim->InvokeWalkRunLeanForTest(Frame, 0.0f);
	TestEqual(TEXT("different CMC with equal epoch cannot inherit lean"), Anim->WalkRunLeanAngleDegrees, 0.0f);
	Anim->InvokeWalkRunLeanForTest(Frame, 0.25f);

	AddExpectedError(TEXT("[Animation][WalkRunLean]"), EAutomationExpectedErrorFlags::Contains, 2);
	Steering.Status = EGGYGOLocomotionSteeringStatus::Invalid;
	Steering.Diagnostic = TEXT("original movement provider retired");
	Anim->InvokeWalkRunLeanForTest(Frame, 0.1f);
	Anim->InvokeWalkRunLeanForTest(Frame, 0.1f);
	TestEqual(TEXT("Invalid retires the original pose value"), Anim->WalkRunLeanAngleDegrees, 0.0f);
	TestFalse(TEXT("Invalid is failure rather than a usable neutral pose"), Anim->bWalkRunLeanPresentationValid);
	TestTrue(TEXT("original provider diagnostic remains visible"), Anim->WalkRunLeanFailureReason.Contains(TEXT("retired")));
	Config.FullLeanYawRateDegreesPerSecond = 0.0f;
	Steering.Status = EGGYGOLocomotionSteeringStatus::Valid;
	Anim->InvokeWalkRunLeanForTest(Frame, 0.1f);
	Anim->InvokeWalkRunLeanForTest(Frame, 0.1f);
	TestTrue(TEXT("missing enabled tuning fails explicitly"), Anim->WalkRunLeanFailureReason.Contains(TEXT("FullLeanYawRate")));
	TestEqual(TEXT("failure does not replace required tuning"), Config.FullLeanYawRateDegreesPerSecond, 0.0f);
	Anim->InvokeNativeUninitializeAnimation();
	TestClearedLifecycleState(*this, *Anim, TEXT("lean lifecycle uninitialize"));
	TestTrue(TEXT("lifecycle preserves authored enable mode"), Config.bEnabled);

	Config.bEnabled = false;
	Anim->SeedDirtyRuntimeStateForTest();
	Anim->InvokeWalkRunLeanForTest(Frame, 0.1f);
	TestFalse(TEXT("explicit disabled mode stops only lean presentation"), Anim->bWalkRunLeanPresentationValid);
	TestTrue(TEXT("disabled mode has no failure diagnostic"), Anim->WalkRunLeanFailureReason.IsEmpty());
	TestEqual(TEXT("lean disable does not clear another presentation resource"), Anim->StateMemory.GaitBlendY, 0.73f);
	Anim->InvokeNativeUninitializeAnimation();
	return true;
}

#endif

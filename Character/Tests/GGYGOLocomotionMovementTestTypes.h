#pragma once

#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "GameFramework/Character.h"

#include "GGYGOLocomotionMovementTestTypes.generated.h"

UCLASS(Transient)
class UGGYGOLocomotionTestMovementComponent : public UGGYGOCharacterMovementComponent
{
	GENERATED_BODY()

public:
	void SetTestAcceleration(const FVector& InAcceleration) { Acceleration = InAcceleration; }
	void SetTestMotion(EGGYGOLocomotionMotionType Type, float Time, EGGYGOStopMotionType Stop = EGGYGOStopMotionType::None)
	{
		LocomotionMotionType = Type;
		LocomotionMotionTime = Time;
		StopMotionType = Stop;
	}
	void AdvanceTestMotion(float DeltaSeconds, bool bHadMoveInput, EGGYGOGait PreviousGait)
	{
		CurrentRootMotion.CleanUpInvalidRootMotion(DeltaSeconds, *CharacterOwner, *this);
		UpdateLocomotionMotion(DeltaSeconds, bHadMoveInput, PreviousGait);
		CurrentRootMotion.PrepareRootMotion(DeltaSeconds, *CharacterOwner, *this, true);
	}
	void AdvanceTestBlend(float DeltaSeconds) { UpdateWalkRunBlend(DeltaSeconds); }
	void SetTestGait(EGGYGOGait Gait) { ResolvedGait = Gait; }
	void SetTestTurnBackPhase(EGGYGOTurnBackPhase Phase) { TurnBackPhase = Phase; }
	/** Inject a previously evaluated sample to check per-update cleanup after rebinding. */
	void SetTestCurveMotion(const FGGYGOLocomotionCurveSample& InSample) { CurveMotion = InSample; }
	void SetAuthorityReplayForTest(bool bEnabled) { bReplayLocomotionFromAuthority = bEnabled; }
	void SetTestSequence(uint16 Sequence) { LocomotionMotionSequence = Sequence; }
	float GetTestMotionTime() const { return LocomotionMotionTime; }
	uint16 GetTestSequence() const { return LocomotionMotionSequence; }
	const FGGYGOLocomotionCurveSample& GetTestCurveMotion() const { return CurveMotion; }
};

UCLASS(Transient)
class AGGYGOLocomotionTestCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AGGYGOLocomotionTestCharacter(const FObjectInitializer& ObjectInitializer);
};

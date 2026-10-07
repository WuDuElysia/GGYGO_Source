#pragma once

#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "Animation/zzzAnim/ZZZAnimInstance.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "GameFramework/Character.h"

#include "GGYGOAnimationLifecycleTestTypes.generated.h"

/** Test-only CMC writer for making owner frames deterministic without production assets. */
UCLASS(Transient)
class UGGYGOAnimationLifecycleTestMovementComponent : public UGGYGOCharacterMovementComponent
{
	GENERATED_BODY()

public:
	void SetAnimationTestFrame(
		const FVector& InVelocity,
		const FVector& InAcceleration,
		EGGYGOGait InGait,
		float InWalkRunBlendAlpha,
		EGGYGOStopMotionType InStopMotionType,
		EGGYGOTurnBackPhase InTurnBackPhase,
		const FVector& InCurveVelocity,
		const FVector& InCurveDirection,
		float InCurveDirectionAngle)
	{
		Velocity = InVelocity;
		Acceleration = InAcceleration;
		ResolvedGait = InGait;
		WalkRunBlendAlpha = InWalkRunBlendAlpha;
		StopMotionType = InStopMotionType;
		TurnBackPhase = InTurnBackPhase;
		CurveMotion.Velocity = InCurveVelocity;
		CurveMotion.Direction = InCurveDirection;
		CurveMotion.DirectionAngle = InCurveDirectionAngle;
	}
};

/** Pawn fixture with an ASC and the test CMC used by animation lifecycle tests. */
UCLASS(Transient)
class AGGYGOAnimationLifecycleTestCharacter : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AGGYGOAnimationLifecycleTestCharacter(const FObjectInitializer& ObjectInitializer);

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override
	{
		return AbilitySystemComponent;
	}

	UPROPERTY()
	TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;
};

/** Test fixture exposing native callbacks and protected animation state to the tests only. */
UCLASS(Transient)
class UGGYGOAnimationLifecycleTestAnimInstance : public UZZZAnimInstance
{
	GENERATED_BODY()

public:
	void SetTestPawnOwner(APawn* InPawn) { TestPawnOwner = InPawn; }
	virtual APawn* TryGetPawnOwner() const override { return TestPawnOwner.Get(); }

	void InvokeNativeInitializeAnimation() { NativeInitializeAnimation(); }
	void InvokeNativeUninitializeAnimation() { NativeUninitializeAnimation(); }
	void InvokeNativeUpdateAnimation(float DeltaSeconds) { NativeUpdateAnimation(DeltaSeconds); }

	void SeedDirtyRuntimeStateForTest();
	/** Consumer unit input only; this does not exercise or impersonate the CMC getter. */
	void InvokeWalkRunLeanForTest(const FGGYGOAnimationStateFrame& InFrame, float DeltaSeconds)
	{
		AnimationState = InFrame;
		UpdateWalkRunLeanPresentation(DeltaSeconds);
	}

	const FZZZAnimSnapshot& GetSnapshotForTest() const { return Snap; }
	const FGGYGOAnimationDebugFrame& GetDebugFrameForTest() const { return GetAnimationDebugFrame(); }

private:
	UPROPERTY(Transient)
	TWeakObjectPtr<APawn> TestPawnOwner;
};

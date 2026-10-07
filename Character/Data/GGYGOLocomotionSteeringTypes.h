#pragma once

#include "CoreMinimal.h"

class ACharacter;
class USceneComponent;
class UGGYGOCharacterMovementComponent;

/** Qualification belongs to Movement; consumers only recover their own presentation. */
enum class EGGYGOLocomotionSteeringStatus : uint8
{
	/** Original configuration initialization is in flight, or accepted dependencies await the first interval. Never movement admission. */
	Initial,
	Valid,
	NotApplicable,
	Invalid
};

/** GT capture of one completed native movement interval. No direction or gait authority. */
struct GGYGO_API FGGYGOLocomotionSteeringSnapshot
{
	EGGYGOLocomotionSteeringStatus Status = EGGYGOLocomotionSteeringStatus::Initial;
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> OriginalMovement;
	TWeakObjectPtr<ACharacter> OriginalCharacter;
	TWeakObjectPtr<USceneComponent> OriginalUpdatedComponent;
	uint64 SourceEpoch = 0;
	uint64 CompletedIntervalSerial = 0;
	uint64 CompletedFrame = 0;
	float NativeDeltaSeconds = 0.0f;
	/** Actual capsule yaw delta / native dt, degrees/s, UE right positive. */
	float ActualSignedYawRate = 0.0f;
	/** Actual OldVelocity -> completed Velocity heading delta / native dt. */
	float ActualSignedVelocityYawRate = 0.0f;
	/** Both original interval endpoint horizontal velocities were finite and nonzero. */
	bool bHasVelocityYawRate = false;
	/** Admitted input heading minus capsule heading, signed degrees. */
	float DesiredDirectionError = 0.0f;
	FString Diagnostic;
};

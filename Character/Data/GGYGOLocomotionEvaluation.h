#pragma once

#include "Character/Data/GGYGOMovementTypes.h"

class UGGYGOLocomotionMotionProfile;

/** Pure interval values. Sample remains unscaled; Scale does not affect Yaw. */
struct FGGYGOLocomotionEvaluationResult
{
	FGGYGOLocomotionCurveSample Sample;
	float ScaledSpeed = 0.0f;
	FVector ScaledVelocity = FVector::ZeroVector;
};

/** EndCyclePosition is an unwrapped mathematical endpoint, not a committed clock. */
struct FGGYGOWalkRunEvaluationResult
{
	FGGYGOLocomotionEvaluationResult Motion;
	float EndCyclePosition = 0.0f;
};

namespace GGYGOLocomotionEvaluation
{
	/**
	 * Delegate asset evaluation to Profile::EvaluateInterval, then check scaling.
	 * Authored zero, tiny positive speed and zero Scale are successful values.
	 * Reset the complete output and optional old Error on entry; publish only on
	 * success. Errors retain the Profile path, interval and native field/reason.
	 * Profiles are borrowed for this call; no state, logging or cache is retained.
	 */
	GGYGO_API bool EvaluateSingleInterval(
		const UGGYGOLocomotionMotionProfile* Profile,
		float StartTime,
		float EndTime,
		float RootMotionScale,
		FGGYGOLocomotionEvaluationResult& OutResult,
		FString* OutError = nullptr);

	/**
	 * Evaluate both required Loop Profiles on one cycle interval, then compose
	 * their values. Alpha must be in [0,1]; Scale and interval inputs must be
	 * finite and non-negative. Alpha endpoints still require BOTH evaluations
	 * to succeed, and retain the selected endpoint's exact unscaled sample.
	 *
	 * Effective period = the two durations blended by Alpha. EndCyclePosition =
	 * StartCyclePosition + AcceptedIntervalSeconds / effective period. A cycle
	 * coordinate can cross multiple periods; this function does not wrap it,
	 * clamp Delta, select a motion, or update a caller's time or phase.
	 *
	 * Positive unscaled speed requires a valid composed direction, even when
	 * Scale is zero. Native asset validation belongs to EvaluateInterval;
	 * this helper checks its own mapping, composition and scaling operations.
	 * Output/Error follow the same atomic reset contract as the single function.
	 */
	GGYGO_API bool EvaluateWalkRunInterval(
		const UGGYGOLocomotionMotionProfile* WalkProfile,
		const UGGYGOLocomotionMotionProfile* RunProfile,
		float StartCyclePosition,
		float AcceptedIntervalSeconds,
		float BlendAlpha,
		float RootMotionScale,
		FGGYGOWalkRunEvaluationResult& OutResult,
		FString* OutError = nullptr);
}

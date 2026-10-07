#pragma once

#include "CoreMinimal.h"

class UGGYGOMovementSet;

/** Stateless angular response math. Native CMC alone owns interval/execution/lifecycle. */
namespace GGYGOLocomotionSteeringEvaluation
{
	GGYGO_API bool ValidateConfiguration(const UGGYGOMovementSet& Set, FString& OutError);
	GGYGO_API bool EvaluateAngularBudget(const UGGYGOMovementSet& Set, float SignedErrorDegrees,
		float WalkRunBlendAlpha, float NativeDeltaSeconds, float& OutBudgetDegrees, FString& OutError);
	GGYGO_API FVector ConstrainPlanarHeading(const FVector& PreviousVelocity, const FVector& NativeVelocity,
		float StartingYawDegrees, float BudgetDegrees);
	GGYGO_API bool CalculateVelocityYawRate(const FVector& OldVelocity, const FVector& NewVelocity,
		float NativeDeltaSeconds, float& OutRateDegreesPerSecond);
}

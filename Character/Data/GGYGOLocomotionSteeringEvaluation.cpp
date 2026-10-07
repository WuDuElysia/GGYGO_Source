#include "Character/Data/GGYGOLocomotionSteeringEvaluation.h"
#include "Character/Data/GGYGOMovementSet.h"

namespace GGYGOLocomotionSteeringEvaluation
{
	bool ValidateConfiguration(const UGGYGOMovementSet& Set, FString& OutError)
	{
		OutError.Reset();
		if (!Set.bEnableLocomotionSteering) return true; // Explicit author-selected mode.
		const auto Fail = [&Set, &OutError](const TCHAR* Reason)
		{
			OutError = FString::Printf(TEXT("MovementSet '%s': LocomotionSteering: %s"), *Set.GetPathName(), Reason);
			return false;
		};
		if (!Set.bOrientRotationToMovement) return Fail(TEXT("requires orient rotation to movement."));
		if (!FMath::IsFinite(Set.SteeringMinYawRate) || !FMath::IsFinite(Set.SteeringMaxYawRate)
			|| Set.SteeringMinYawRate < 0.0f || Set.SteeringMaxYawRate <= 0.0f
			|| Set.SteeringMinYawRate > Set.SteeringMaxYawRate)
			return Fail(TEXT("yaw rate bounds must be finite, 0 <= min <= max, and max > 0 (degrees/s)."));
		if (!FMath::IsFinite(Set.SteeringWalkAngularRateScale) || !FMath::IsFinite(Set.SteeringRunAngularRateScale)
			|| Set.SteeringWalkAngularRateScale <= 0.0f || Set.SteeringRunAngularRateScale <= 0.0f)
			return Fail(TEXT("Walk/Run angular rate scales must be finite and positive."));
		const FRichCurve* Curve = Set.SteeringAngleResponse.GetRichCurveConst();
		if (!Curve || Curve->GetNumKeys() < 2) return Fail(TEXT("required angle response curve is missing; author keys covering 0..180 degrees."));
		float PreviousTime = -1.0f;
		for (const FRichCurveKey& Key : Curve->GetConstRefOfKeys())
		{
			if (!FMath::IsFinite(Key.Time) || !FMath::IsFinite(Key.Value)
				|| !FMath::IsFinite(Key.ArriveTangent) || !FMath::IsFinite(Key.LeaveTangent)
				|| !FMath::IsFinite(Key.ArriveTangentWeight) || !FMath::IsFinite(Key.LeaveTangentWeight)
				|| (Key.InterpMode != RCIM_Linear && Key.InterpMode != RCIM_Constant && Key.InterpMode != RCIM_Cubic)
				|| Key.TangentWeightMode < RCTWM_WeightedNone || Key.TangentWeightMode > RCTWM_WeightedBoth
				|| Key.ArriveTangentWeight < 0.0f || Key.LeaveTangentWeight < 0.0f
				|| Key.Time < 0.0f || Key.Time > 180.0f || Key.Time <= PreviousTime
				|| Key.Value < 0.0f || Key.Value > 1.0f)
				return Fail(TEXT("response keys/interpolation/tangents must be valid and finite, times strictly increasing in 0..180, values in 0..1."));
			PreviousTime = Key.Time;
		}
		const auto& Keys = Curve->GetConstRefOfKeys();
		if (Keys[0].Time != 0.0f || Keys.Last().Time != 180.0f)
			return Fail(TEXT("response curve must explicitly cover endpoints 0 and 180 degrees; no implicit extrapolation."));
		return true;
	}

	bool EvaluateAngularBudget(const UGGYGOMovementSet& Set, float SignedErrorDegrees,
		float WalkRunBlendAlpha, float NativeDeltaSeconds, float& OutBudgetDegrees, FString& OutError)
	{
		OutBudgetDegrees = 0.0f;
		if (!ValidateConfiguration(Set, OutError)) return false;
		const auto Fail = [&Set, &OutError](const TCHAR* Reason)
		{
			OutError = FString::Printf(TEXT("MovementSet '%s': LocomotionSteering evaluation: %s"), *Set.GetPathName(), Reason);
			return false;
		};
		if (!Set.bEnableLocomotionSteering) return Fail(TEXT("steering is explicitly disabled."));
		if (!FMath::IsFinite(SignedErrorDegrees) || FMath::Abs(SignedErrorDegrees) > 180.0f
			|| !FMath::IsFinite(WalkRunBlendAlpha) || WalkRunBlendAlpha < 0.0f || WalkRunBlendAlpha > 1.0f
			|| !FMath::IsFinite(NativeDeltaSeconds) || NativeDeltaSeconds <= 0.0f)
			return Fail(TEXT("requires signed error in [-180,180], Alpha in [0,1], positive finite native dt."));
		const float Response = Set.SteeringAngleResponse.GetRichCurveConst()->Eval(FMath::Abs(SignedErrorDegrees));
		if (!FMath::IsFinite(Response) || Response < 0.0f || Response > 1.0f)
			return Fail(TEXT("authored curve evaluated outside finite [0,1]; check cubic tangents/weights."));
		const float Scale = FMath::Lerp(Set.SteeringWalkAngularRateScale, Set.SteeringRunAngularRateScale, WalkRunBlendAlpha);
		const float UnboundedRate = FMath::Lerp(Set.SteeringMinYawRate, Set.SteeringMaxYawRate, Response) * Scale;
		if (!FMath::IsFinite(UnboundedRate) || !FMath::IsFinite(UnboundedRate * NativeDeltaSeconds))
			return Fail(TEXT("angular rate or native interval budget overflowed."));
		const float Rate = FMath::Clamp(UnboundedRate, Set.SteeringMinYawRate, Set.SteeringMaxYawRate);
		OutBudgetDegrees = FMath::Min(Rate * NativeDeltaSeconds, 180.0f);
		return true;
	}

	FVector ConstrainPlanarHeading(const FVector& PreviousVelocity, const FVector& NativeVelocity,
		float StartingYawDegrees, float BudgetDegrees)
	{
		const double Speed = NativeVelocity.Size2D();
		if (Speed <= UE_KINDA_SMALL_NUMBER) return NativeVelocity;
		const float PreviousYaw = PreviousVelocity.SizeSquared2D() > UE_KINDA_SMALL_NUMBER
			? PreviousVelocity.Rotation().Yaw : StartingYawDegrees;
		const float Error = FMath::FindDeltaAngleDegrees(PreviousYaw, NativeVelocity.Rotation().Yaw);
		const float Yaw = PreviousYaw + FMath::Clamp(Error, -BudgetDegrees, BudgetDegrees);
		const FVector Direction = FRotator(0.0f, Yaw, 0.0f).Vector();
		return FVector(Direction.X * Speed, Direction.Y * Speed, NativeVelocity.Z);
	}

	bool CalculateVelocityYawRate(const FVector& OldVelocity, const FVector& NewVelocity,
		float NativeDeltaSeconds, float& OutRateDegreesPerSecond)
	{
		OutRateDegreesPerSecond = 0.0f;
		if (OldVelocity.ContainsNaN() || NewVelocity.ContainsNaN()
			|| OldVelocity.SizeSquared2D() <= UE_KINDA_SMALL_NUMBER || NewVelocity.SizeSquared2D() <= UE_KINDA_SMALL_NUMBER
			|| !FMath::IsFinite(NativeDeltaSeconds) || NativeDeltaSeconds <= 0.0f) return false;
		OutRateDegreesPerSecond = FMath::FindDeltaAngleDegrees(OldVelocity.Rotation().Yaw, NewVelocity.Rotation().Yaw) / NativeDeltaSeconds;
		return FMath::IsFinite(OutRateDegreesPerSecond);
	}
}

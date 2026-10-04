#include "Character/Data/GGYGOLocomotionEvaluation.h"

#include "Character/Data/GGYGOLocomotionMotionProfile.h"

namespace
{
	/** Stack-only error inputs; format object paths and intervals only on failure. */
	struct FEvaluationErrorContext
	{
		const UGGYGOLocomotionMotionProfile* FirstProfile;
		const UGGYGOLocomotionMotionProfile* SecondProfile;
		float Start;
		float EndOrSeconds;
		float BlendAlpha;
		float RootMotionScale;
		bool bWalkRun;
		float FirstStartTime = 0.0f;
		float FirstEndTime = 0.0f;
		float SecondStartTime = 0.0f;
		float SecondEndTime = 0.0f;
		bool bHasMappedIntervals = false;

		bool Fail(FString* OutError, const TCHAR* Field, const FString& Reason) const
		{
			if (OutError)
			{
				FString Inputs = bWalkRun
					? FString::Printf(TEXT("WalkRun WalkLoopProfile='%s', RunLoopProfile='%s', StartCyclePosition=%.9g, AcceptedIntervalSeconds=%.9g, BlendAlpha=%.9g, RootMotionScale=%.9g"),
						*GetPathNameSafe(FirstProfile), *GetPathNameSafe(SecondProfile),
						Start, EndOrSeconds, BlendAlpha, RootMotionScale)
					: FString::Printf(TEXT("SingleInterval Profile='%s', Interval=[%.9g, %.9g], RootMotionScale=%.9g"),
						*GetPathNameSafe(FirstProfile), Start, EndOrSeconds, RootMotionScale);
				if (bHasMappedIntervals)
				{
					Inputs += FString::Printf(TEXT(", WalkInterval=[%.9g, %.9g], RunInterval=[%.9g, %.9g]"),
						FirstStartTime, FirstEndTime, SecondStartTime, SecondEndTime);
				}
				*OutError = FString::Printf(TEXT("%s: %s: %s"), *Inputs, Field, *Reason);
			}
			return false;
		}

		bool FailProfile(FString* OutError, const TCHAR* Side,
			float StartTime, float EndTime, const FString& NativeError) const
		{
			if (!OutError)
			{
				return false;
			}
			return Fail(OutError, Side, FString::Printf(TEXT("interval [%.9g, %.9g]: %s"),
				StartTime, EndTime, *NativeError));
		}
	};

	bool IsFiniteVector(const FVector& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	bool CheckNonNegativeInput(float Value, const TCHAR* Field,
		const FEvaluationErrorContext& Context, FString* OutError)
	{
		return (FMath::IsFinite(Value) && Value >= 0.0f)
			|| Context.Fail(OutError, Field, TEXT("must be finite and non-negative"));
	}

	bool TryStoreFloat(double Value, float& OutValue, const TCHAR* Field,
		const FEvaluationErrorContext& Context, FString* OutError)
	{
		const double Maximum = static_cast<double>(TNumericLimits<float>::Max());
		if (!FMath::IsFinite(Value) || Value > Maximum || Value < -Maximum)
		{
			return Context.Fail(OutError, Field, TEXT("computed value is non-finite or outside float range"));
		}
		const float Candidate = static_cast<float>(Value);
		if (!FMath::IsFinite(Candidate))
		{
			return Context.Fail(OutError, Field, TEXT("stored float value is non-finite"));
		}
		OutValue = Candidate;
		return true;
	}

	double BlendScalar(float WalkValue, float RunValue, float Alpha)
	{
		// Weighted doubles avoid float difference overflow and retain exact
		// endpoints even when the two finite durations have very different sizes.
		const double Weight = static_cast<double>(Alpha);
		return static_cast<double>(WalkValue) * (1.0 - Weight)
			+ static_cast<double>(RunValue) * Weight;
	}

	bool BuildScaledResult(const FGGYGOLocomotionCurveSample& Sample,
		float RootMotionScale, FGGYGOLocomotionEvaluationResult& OutCandidate,
		const FEvaluationErrorContext& Context, FString* OutError)
	{
		// Preserve the existing float Speed*Scale contract, including valid zero.
		const float ScaledSpeed = Sample.Speed * RootMotionScale;
		if (!FMath::IsFinite(ScaledSpeed) || ScaledSpeed < 0.0f)
		{
			return Context.Fail(OutError, TEXT("ScaledSpeed"),
				TEXT("Speed * RootMotionScale must produce a finite non-negative float"));
		}
		const FVector ScaledVelocity = Sample.Direction * ScaledSpeed;
		if (!IsFiniteVector(ScaledVelocity))
		{
			return Context.Fail(OutError, TEXT("ScaledVelocity"),
				TEXT("direction * ScaledSpeed produced a non-finite vector"));
		}

		OutCandidate.Sample = Sample;
		OutCandidate.ScaledSpeed = ScaledSpeed;
		OutCandidate.ScaledVelocity = ScaledVelocity;
		return true;
	}
}

bool GGYGOLocomotionEvaluation::EvaluateSingleInterval(
	const UGGYGOLocomotionMotionProfile* Profile,
	float StartTime,
	float EndTime,
	float RootMotionScale,
	FGGYGOLocomotionEvaluationResult& OutResult,
	FString* OutError)
{
	OutResult = FGGYGOLocomotionEvaluationResult();
	if (OutError)
	{
		OutError->Reset();
	}
	const FEvaluationErrorContext Context{Profile, nullptr, StartTime, EndTime, 0.0f, RootMotionScale, false};
	if (!IsValid(Profile))
	{
		return Context.Fail(OutError, TEXT("Profile"), TEXT("must reference a valid Profile object"));
	}
	if (!CheckNonNegativeInput(RootMotionScale, TEXT("RootMotionScale"), Context, OutError))
	{
		return false;
	}

	FGGYGOLocomotionCurveSample Sample;
	FString ProfileError;
	if (!Profile->EvaluateInterval(StartTime, EndTime, Sample, OutError ? &ProfileError : nullptr))
	{
		return Context.FailProfile(OutError, TEXT("Profile"), StartTime, EndTime, ProfileError);
	}
	FGGYGOLocomotionEvaluationResult Candidate;
	if (!BuildScaledResult(Sample, RootMotionScale, Candidate, Context, OutError))
	{
		return false;
	}
	OutResult = Candidate;
	return true;
}

bool GGYGOLocomotionEvaluation::EvaluateWalkRunInterval(
	const UGGYGOLocomotionMotionProfile* WalkProfile,
	const UGGYGOLocomotionMotionProfile* RunProfile,
	float StartCyclePosition,
	float AcceptedIntervalSeconds,
	float BlendAlpha,
	float RootMotionScale,
	FGGYGOWalkRunEvaluationResult& OutResult,
	FString* OutError)
{
	OutResult = FGGYGOWalkRunEvaluationResult();
	if (OutError)
	{
		OutError->Reset();
	}
	const FEvaluationErrorContext Context{WalkProfile, RunProfile, StartCyclePosition,
		AcceptedIntervalSeconds, BlendAlpha, RootMotionScale, true};
	if (!IsValid(WalkProfile))
	{
		return Context.Fail(OutError, TEXT("WalkLoopProfile"), TEXT("must reference a valid Profile object"));
	}
	if (!IsValid(RunProfile))
	{
		return Context.Fail(OutError, TEXT("RunLoopProfile"), TEXT("must reference a valid Profile object"));
	}
	if (!WalkProfile->bLoop)
	{
		return Context.Fail(OutError, TEXT("WalkLoopProfile.bLoop"), TEXT("must be true for a WalkRun interval"));
	}
	if (!RunProfile->bLoop)
	{
		return Context.Fail(OutError, TEXT("RunLoopProfile.bLoop"), TEXT("must be true for a WalkRun interval"));
	}
	if (!CheckNonNegativeInput(StartCyclePosition, TEXT("StartCyclePosition"), Context, OutError)
		|| !CheckNonNegativeInput(AcceptedIntervalSeconds, TEXT("AcceptedIntervalSeconds"), Context, OutError)
		|| !CheckNonNegativeInput(RootMotionScale, TEXT("RootMotionScale"), Context, OutError))
	{
		return false;
	}
	if (!FMath::IsFinite(BlendAlpha) || BlendAlpha < 0.0f || BlendAlpha > 1.0f)
	{
		return Context.Fail(OutError, TEXT("BlendAlpha"), TEXT("must be finite and in [0,1]"));
	}

	const float WalkDuration = WalkProfile->Duration;
	const float RunDuration = RunProfile->Duration;
	// Only protect the arithmetic mapping. EvaluateInterval retains the asset's
	// minimum-duration rule and all curve/key validation; do not duplicate it.
	if (!FMath::IsFinite(WalkDuration) || WalkDuration <= 0.0f)
	{
		return Context.Fail(OutError, TEXT("WalkLoopProfile.Duration"),
			TEXT("cycle-coordinate mapping requires a finite positive duration"));
	}
	if (!FMath::IsFinite(RunDuration) || RunDuration <= 0.0f)
	{
		return Context.Fail(OutError, TEXT("RunLoopProfile.Duration"),
			TEXT("cycle-coordinate mapping requires a finite positive duration"));
	}

	float EffectiveDuration = 0.0f;
	if (!TryStoreFloat(BlendScalar(WalkDuration, RunDuration, BlendAlpha), EffectiveDuration,
		TEXT("EffectiveLoopDuration"), Context, OutError))
	{
		return false;
	}
	if (EffectiveDuration <= 0.0f)
	{
		return Context.Fail(OutError, TEXT("EffectiveLoopDuration"), TEXT("must be positive before division"));
	}
	FGGYGOWalkRunEvaluationResult Candidate;
	const double EndCycle = static_cast<double>(StartCyclePosition)
		+ static_cast<double>(AcceptedIntervalSeconds) / static_cast<double>(EffectiveDuration);
	if (!TryStoreFloat(EndCycle, Candidate.EndCyclePosition, TEXT("EndCyclePosition"), Context, OutError))
	{
		return false;
	}

	float WalkStart = 0.0f;
	float WalkEnd = 0.0f;
	float RunStart = 0.0f;
	float RunEnd = 0.0f;
	if (!TryStoreFloat(static_cast<double>(StartCyclePosition) * WalkDuration, WalkStart,
			TEXT("WalkLoopProfile.StartTime"), Context, OutError)
		|| !TryStoreFloat(static_cast<double>(Candidate.EndCyclePosition) * WalkDuration, WalkEnd,
			TEXT("WalkLoopProfile.EndTime"), Context, OutError)
		|| !TryStoreFloat(static_cast<double>(StartCyclePosition) * RunDuration, RunStart,
			TEXT("RunLoopProfile.StartTime"), Context, OutError)
		|| !TryStoreFloat(static_cast<double>(Candidate.EndCyclePosition) * RunDuration, RunEnd,
			TEXT("RunLoopProfile.EndTime"), Context, OutError))
	{
		return false;
	}
	FEvaluationErrorContext MappedContext = Context;
	MappedContext.FirstStartTime = WalkStart;
	MappedContext.FirstEndTime = WalkEnd;
	MappedContext.SecondStartTime = RunStart;
	MappedContext.SecondEndTime = RunEnd;
	MappedContext.bHasMappedIntervals = true;

	FGGYGOLocomotionCurveSample WalkSample;
	FGGYGOLocomotionCurveSample RunSample;
	FString ProfileError;
	if (!WalkProfile->EvaluateInterval(WalkStart, WalkEnd, WalkSample, OutError ? &ProfileError : nullptr))
	{
		return MappedContext.FailProfile(OutError, TEXT("WalkLoopProfile"), WalkStart, WalkEnd, ProfileError);
	}
	if (!RunProfile->EvaluateInterval(RunStart, RunEnd, RunSample, OutError ? &ProfileError : nullptr))
	{
		return MappedContext.FailProfile(OutError, TEXT("RunLoopProfile"), RunStart, RunEnd, ProfileError);
	}

	FGGYGOLocomotionCurveSample Sample;
	if (BlendAlpha == 0.0f)
	{
		Sample = WalkSample;
	}
	else if (BlendAlpha == 1.0f)
	{
		Sample = RunSample;
	}
	else
	{
		if (!TryStoreFloat(BlendScalar(WalkSample.Speed, RunSample.Speed, BlendAlpha), Sample.Speed,
				TEXT("Mixed.Speed"), MappedContext, OutError)
			|| !TryStoreFloat(BlendScalar(WalkSample.YawDeltaDegrees, RunSample.YawDeltaDegrees, BlendAlpha),
				Sample.YawDeltaDegrees, TEXT("Mixed.YawDeltaDegrees"), MappedContext, OutError)
			|| !TryStoreFloat(BlendScalar(WalkSample.YawTotalDegrees, RunSample.YawTotalDegrees, BlendAlpha),
				Sample.YawTotalDegrees, TEXT("Mixed.YawTotalDegrees"), MappedContext, OutError))
		{
			return false;
		}
		if (Sample.Speed < 0.0f)
		{
			return MappedContext.Fail(OutError, TEXT("Mixed.Speed"), TEXT("must be non-negative"));
		}
		const double Alpha = static_cast<double>(BlendAlpha);
		const FVector BlendedDirection = WalkSample.Direction * (1.0 - Alpha) + RunSample.Direction * Alpha;
		if (!IsFiniteVector(BlendedDirection))
		{
			return MappedContext.Fail(OutError, TEXT("Mixed.Direction"), TEXT("composed direction is non-finite"));
		}
		Sample.Direction = BlendedDirection.GetSafeNormal2D();
		if (!IsFiniteVector(Sample.Direction))
		{
			return MappedContext.Fail(OutError, TEXT("Mixed.Direction"), TEXT("normalized composed direction is non-finite"));
		}
		const bool bHasDirection = !Sample.Direction.IsNearlyZero(KINDA_SMALL_NUMBER);
		if (Sample.Speed > 0.0f && !bHasDirection)
		{
			return MappedContext.Fail(OutError, TEXT("Mixed.Direction"),
				TEXT("positive unscaled speed requires a valid composed direction, including at zero Scale"));
		}
		Sample.Velocity = Sample.Direction * Sample.Speed;
		if (!IsFiniteVector(Sample.Velocity))
		{
			return MappedContext.Fail(OutError, TEXT("Mixed.Velocity"), TEXT("composed velocity is non-finite"));
		}
		const double Angle = bHasDirection
			? FMath::RadiansToDegrees(FMath::Atan2(Sample.Direction.Y, Sample.Direction.X)) : 0.0;
		if (!TryStoreFloat(Angle, Sample.DirectionAngle, TEXT("Mixed.DirectionAngle"), MappedContext, OutError))
		{
			return false;
		}
		Sample.ClipLength = EffectiveDuration;
		Sample.bLoopClip = true;
		Sample.bHasAuthoredDirection = bHasDirection;
		Sample.bHasCurveSource = true;
		// Current Profile evaluation produces no PositionDelta; default zero/false
		// preserves that contract without fabricating an integrated displacement.
	}
	if (!BuildScaledResult(Sample, RootMotionScale, Candidate.Motion, MappedContext, OutError))
	{
		return false;
	}
	OutResult = Candidate;
	return true;
}

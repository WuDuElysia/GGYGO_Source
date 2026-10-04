#include "Character/Data/GGYGOLocomotionMotionProfile.h"

#include "Curves/RichCurve.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

namespace
{
	const FRichCurve* GetCurveData(const FRuntimeFloatCurve& RuntimeCurve)
	{
		return RuntimeCurve.GetRichCurveConst();
	}

	bool FailEvaluation(FString* OutError, const TCHAR* Field, const TCHAR* Reason)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("%s: %s."), Field, Reason);
		}
		return false;
	}

	bool TryEvaluateCurve(const FRichCurve& Curve, const float Time,
		const TCHAR* CurveLabel, float& OutValue, FString* OutError)
	{
		OutValue = Curve.Eval(Time);
		return FMath::IsFinite(OutValue)
			|| FailEvaluation(OutError, CurveLabel, TEXT("evaluated value is non-finite"));
	}

	bool HasCurveKeys(const FRichCurve* Curve)
	{
		return Curve && Curve->GetConstRefOfKeys().Num() > 0;
	}

	bool GetLoopPosition(const float Time, const float Duration, double& OutCycle, float& OutPhase)
	{
		const double TimeAsDouble = static_cast<double>(Time);
		const double DurationAsDouble = static_cast<double>(Duration);
		OutCycle = FMath::FloorToDouble(TimeAsDouble / DurationAsDouble);
		double Phase = TimeAsDouble - OutCycle * DurationAsDouble;
		if (!FMath::IsFinite(OutCycle) || !FMath::IsFinite(Phase))
		{
			return false;
		}

		// Guard against floating-point roundoff at an exact cycle boundary.
		if (Phase < 0.0)
		{
			if (Phase < -DurationAsDouble)
			{
				return false;
			}
			Phase += DurationAsDouble;
			OutCycle -= 1.0;
		}
		else if (Phase >= DurationAsDouble)
		{
			if (Phase >= 2.0 * DurationAsDouble)
			{
				return false;
			}
			Phase -= DurationAsDouble;
			OutCycle += 1.0;
		}

		OutPhase = static_cast<float>(Phase);
		if (OutPhase == Duration)
		{
			// The double remainder can round up to Duration when converted to float.
			OutPhase = 0.0f;
			OutCycle += 1.0;
		}
		return FMath::IsFinite(OutCycle) && OutCycle >= 0.0
			&& FMath::IsFinite(OutPhase) && OutPhase >= 0.0f && OutPhase < Duration;
	}

	bool TryEvaluateUnwrappedYaw(
		const FRichCurve& YawCurve,
		const float Time,
		const float Duration,
		const TCHAR* PhaseLabel,
		float& OutYaw,
		FString* OutError)
	{
		double Cycle = 0.0;
		float Phase = 0.0f;
		if (!GetLoopPosition(Time, Duration, Cycle, Phase))
		{
			return FailEvaluation(OutError, PhaseLabel, TEXT("cannot resolve a finite loop cycle/phase"));
		}

		float OriginYaw = 0.0f;
		float CycleEndYaw = 0.0f;
		float PhaseYaw = 0.0f;
		if (!TryEvaluateCurve(YawCurve, 0.0f, TEXT("YawCurve.loopOrigin"), OriginYaw, OutError)
			|| !TryEvaluateCurve(YawCurve, Duration, TEXT("YawCurve.loopEnd"), CycleEndYaw, OutError)
			|| !TryEvaluateCurve(YawCurve, Phase, PhaseLabel, PhaseYaw, OutError))
		{
			return false;
		}

		const double CycleYawDelta =
			static_cast<double>(CycleEndYaw) - static_cast<double>(OriginYaw);
		const double UnwrappedYaw =
			Cycle * CycleYawDelta + static_cast<double>(PhaseYaw);
		if (!FMath::IsFinite(CycleYawDelta) || !FMath::IsFinite(UnwrappedYaw)
			|| UnwrappedYaw > static_cast<double>(TNumericLimits<float>::Max())
			|| UnwrappedYaw < -static_cast<double>(TNumericLimits<float>::Max()))
		{
			return FailEvaluation(OutError, PhaseLabel, TEXT("unwrapped YawCurve value is non-finite or exceeds float range"));
		}

		OutYaw = static_cast<float>(UnwrappedYaw);
		return FMath::IsFinite(OutYaw)
			|| FailEvaluation(OutError, PhaseLabel, TEXT("unwrapped YawCurve value is non-finite"));
	}

	bool ValidateCurve(const FRichCurve* Curve, const float Duration, const TCHAR* CurveLabel,
		FString& OutError, const bool bRequireNonNegativeValues = false)
	{
		if (!HasCurveKeys(Curve))
		{
			OutError = FString::Printf(TEXT("%s must contain at least one key."), CurveLabel);
			return false;
		}

		if (Curve->PreInfinityExtrap < RCCE_Cycle || Curve->PreInfinityExtrap > RCCE_None)
		{
			return FailEvaluation(&OutError, CurveLabel, TEXT("PreInfinityExtrap has an invalid enum value"));
		}
		if (Curve->PostInfinityExtrap < RCCE_Cycle || Curve->PostInfinityExtrap > RCCE_None)
		{
			return FailEvaluation(&OutError, CurveLabel, TEXT("PostInfinityExtrap has an invalid enum value"));
		}

		const TArray<FRichCurveKey>& Keys = Curve->GetConstRefOfKeys();
		for (int32 KeyIndex = 0; KeyIndex < Keys.Num(); ++KeyIndex)
		{
			const FRichCurveKey& Key = Keys[KeyIndex];
			const auto FailKey = [&](const TCHAR* Field, const TCHAR* Reason)
			{
				OutError = FString::Printf(TEXT("%s.Keys[%d].%s: %s."), CurveLabel, KeyIndex, Field, Reason);
				return false;
			};
			if (!FMath::IsFinite(Key.Time))
			{
				return FailKey(TEXT("Time"), TEXT("must be finite"));
			}
			if (KeyIndex > 0 && Key.Time <= Keys[KeyIndex - 1].Time)
			{
				return FailKey(TEXT("Time"), TEXT("must be strictly increasing"));
			}
			if (!FMath::IsFinite(Key.Value))
			{
				return FailKey(TEXT("Value"), TEXT("must be finite"));
			}
			if (bRequireNonNegativeValues && Key.Value < 0.0f)
			{
				return FailKey(TEXT("Value"), TEXT("speed must be non-negative"));
			}
			if (!FMath::IsFinite(Key.ArriveTangent))
			{
				return FailKey(TEXT("ArriveTangent"), TEXT("must be finite"));
			}
			if (!FMath::IsFinite(Key.LeaveTangent))
			{
				return FailKey(TEXT("LeaveTangent"), TEXT("must be finite"));
			}
			if (Key.InterpMode < RCIM_Linear || Key.InterpMode > RCIM_None)
			{
				return FailKey(TEXT("InterpMode"), TEXT("has an invalid enum value"));
			}
			if (Key.TangentMode < RCTM_Auto || Key.TangentMode > RCTM_SmartAuto)
			{
				return FailKey(TEXT("TangentMode"), TEXT("has an invalid enum value"));
			}
			if (Key.TangentWeightMode < RCTWM_WeightedNone || Key.TangentWeightMode > RCTWM_WeightedBoth)
			{
				return FailKey(TEXT("TangentWeightMode"), TEXT("has an invalid enum value"));
			}
			const bool bWeightedArrive = Key.TangentWeightMode == RCTWM_WeightedArrive
				|| Key.TangentWeightMode == RCTWM_WeightedBoth;
			const bool bWeightedLeave = Key.TangentWeightMode == RCTWM_WeightedLeave
				|| Key.TangentWeightMode == RCTWM_WeightedBoth;
			if (bWeightedArrive && (!FMath::IsFinite(Key.ArriveTangentWeight) || Key.ArriveTangentWeight < 0.0f))
			{
				return FailKey(TEXT("ArriveTangentWeight"), TEXT("active tangent weight must be finite and non-negative"));
			}
			if (bWeightedLeave && (!FMath::IsFinite(Key.LeaveTangentWeight) || Key.LeaveTangentWeight < 0.0f))
			{
				return FailKey(TEXT("LeaveTangentWeight"), TEXT("active tangent weight must be finite and non-negative"));
			}
		}

		// A single key intentionally represents a constant curve. Multi-key curves
		// should cover the effective clip interval to avoid accidental extrapolation.
		if (Keys.Num() > 1
			&& (Keys[0].Time > 0.0001f || Keys.Last().Time < Duration - 0.0001f))
		{
			OutError = FString::Printf(TEXT("%s must cover [0, Duration]."), CurveLabel);
			return false;
		}

		return true;
	}
}

bool UGGYGOLocomotionMotionProfile::EvaluateInterval(
	const float StartTime,
	const float EndTime,
	FGGYGOLocomotionCurveSample& OutSample,
	FString* OutError) const
{
	OutSample.Reset();
	if (OutError)
	{
		OutError->Reset();
	}

	if (!FMath::IsFinite(StartTime) || StartTime < 0.0f)
	{
		return FailEvaluation(OutError, TEXT("StartTime"), TEXT("must be finite and non-negative"));
	}
	if (!FMath::IsFinite(EndTime) || EndTime < 0.0f)
	{
		return FailEvaluation(OutError, TEXT("EndTime"), TEXT("must be finite and non-negative"));
	}
	if (EndTime < StartTime)
	{
		return FailEvaluation(OutError, TEXT("EndTime"), TEXT("must not precede StartTime"));
	}

	FString ValidationError;
	if (!ValidateProfile(ValidationError))
	{
		if (OutError)
		{
			*OutError = MoveTemp(ValidationError);
		}
		return false;
	}

	const FRichCurve* Speed = GetCurveData(SpeedCurve);
	const FRichCurve* DirectionX = GetCurveData(DirectionXCurve);
	const FRichCurve* DirectionY = GetCurveData(DirectionYCurve);
	const FRichCurve* Yaw = GetCurveData(YawCurve);
	float SampleTime = 0.0f;
	float StartYawTime = 0.0f;
	float StartYaw = 0.0f;
	float EndYaw = 0.0f;
	if (bLoop)
	{
		double EndCycle = 0.0;
		if (!GetLoopPosition(EndTime, Duration, EndCycle, SampleTime))
		{
			return FailEvaluation(OutError, TEXT("EndTime"), TEXT("cannot resolve a finite loop cycle/phase"));
		}
		if (!TryEvaluateUnwrappedYaw(*Yaw, StartTime, Duration, TEXT("YawCurve.loopPhase(StartTime)"), StartYaw, OutError)
			|| !TryEvaluateUnwrappedYaw(*Yaw, EndTime, Duration, TEXT("YawCurve.loopPhase(EndTime)"), EndYaw, OutError))
		{
			return false;
		}
	}
	else
	{
		StartYawTime = FMath::Clamp(StartTime, 0.0f, Duration);
		SampleTime = FMath::Clamp(EndTime, 0.0f, Duration);
		if (!TryEvaluateCurve(*Yaw, StartYawTime, TEXT("YawCurve(StartTime)"), StartYaw, OutError)
			|| !TryEvaluateCurve(*Yaw, SampleTime, TEXT("YawCurve(EndTime)"), EndYaw, OutError))
		{
			return false;
		}
	}

	float RawSpeed = 0.0f;
	float RawDirectionX = 0.0f;
	float RawDirectionY = 0.0f;
	if (!TryEvaluateCurve(*Speed, SampleTime, TEXT("SpeedCurve"), RawSpeed, OutError)
		|| !TryEvaluateCurve(*DirectionX, SampleTime, TEXT("DirectionXCurve"), RawDirectionX, OutError)
		|| !TryEvaluateCurve(*DirectionY, SampleTime, TEXT("DirectionYCurve"), RawDirectionY, OutError))
	{
		return false;
	}
	if (RawSpeed < 0.0f)
	{
		return FailEvaluation(OutError, TEXT("SpeedCurve"), TEXT("evaluated speed must be non-negative"));
	}

	const FVector RawDirection(RawDirectionX, RawDirectionY, 0.0f);
	const FVector Direction = RawDirection.GetSafeNormal2D();
	if (!FMath::IsFinite(Direction.X) || !FMath::IsFinite(Direction.Y) || !FMath::IsFinite(Direction.Z))
	{
		return FailEvaluation(OutError, TEXT("DirectionXCurve/DirectionYCurve"), TEXT("normalized direction is non-finite"));
	}
	const bool bHasDirection = !Direction.IsNearlyZero(KINDA_SMALL_NUMBER);
	if (RawSpeed > 0.0f && !bHasDirection)
	{
		return FailEvaluation(OutError, TEXT("DirectionXCurve/DirectionYCurve"), TEXT("positive SpeedCurve value requires a valid direction"));
	}

	const float YawDelta = EndYaw - StartYaw;
	if (!FMath::IsFinite(YawDelta))
	{
		return FailEvaluation(OutError, TEXT("YawDeltaDegrees"), TEXT("YawCurve interval difference is non-finite"));
	}

	FGGYGOLocomotionCurveSample Candidate;
	Candidate.Speed = RawSpeed;
	Candidate.Direction = Direction;
	Candidate.Velocity = Direction * RawSpeed;
	if (!FMath::IsFinite(Candidate.Velocity.X) || !FMath::IsFinite(Candidate.Velocity.Y)
		|| !FMath::IsFinite(Candidate.Velocity.Z))
	{
		return FailEvaluation(OutError, TEXT("Velocity"), TEXT("SpeedCurve/direction product is non-finite"));
	}
	Candidate.DirectionAngle = bHasDirection
		? FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X))
		: 0.0f;
	if (!FMath::IsFinite(Candidate.DirectionAngle))
	{
		return FailEvaluation(OutError, TEXT("DirectionAngle"), TEXT("DirectionXCurve/DirectionYCurve angle is non-finite"));
	}
	Candidate.YawTotalDegrees = EndYaw;
	Candidate.YawDeltaDegrees = YawDelta;
	Candidate.ClipLength = Duration;
	Candidate.bLoopClip = bLoop;
	Candidate.bHasAuthoredDirection = bHasDirection;
	Candidate.bHasPositionDelta = false;
	Candidate.bHasCurveSource = true;
	OutSample = Candidate;
	return true;
}

bool UGGYGOLocomotionMotionProfile::ValidateProfile(FString& OutError) const
{
	OutError.Reset();
	if (!FMath::IsFinite(Duration) || Duration <= UE_SMALL_NUMBER)
	{
		return FailEvaluation(&OutError, TEXT("Duration"), TEXT("must be finite and greater than UE_SMALL_NUMBER"));
	}

	return ValidateCurve(GetCurveData(SpeedCurve), Duration, TEXT("SpeedCurve"), OutError, true)
		&& ValidateCurve(GetCurveData(DirectionXCurve), Duration, TEXT("DirectionXCurve"), OutError)
		&& ValidateCurve(GetCurveData(DirectionYCurve), Duration, TEXT("DirectionYCurve"), OutError)
		&& ValidateCurve(GetCurveData(YawCurve), Duration, TEXT("YawCurve"), OutError);
}

#if WITH_EDITOR
EDataValidationResult UGGYGOLocomotionMotionProfile::IsDataValid(FDataValidationContext& Context) const
{
	const EDataValidationResult ParentResult = Super::IsDataValid(Context);
	FString Error;
	if (!ValidateProfile(Error))
	{
		Context.AddError(FText::FromString(Error));
		return EDataValidationResult::Invalid;
	}
	return ParentResult == EDataValidationResult::Invalid ? ParentResult : EDataValidationResult::Valid;
}
#endif

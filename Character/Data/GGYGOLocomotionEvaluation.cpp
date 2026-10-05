#include "Character/Data/GGYGOLocomotionEvaluation.h"
#include "Character/Data/GGYGOAnimationSourceCurveEvaluation.h"

#include "Character/Data/GGYGOLocomotionMotionProfile.h"
#include "Animation/BlendSpace1D.h"

namespace GGYGOLocomotionSourceEvaluation
{
	const FName SpeedName(TEXT("RootMotion_Speed"));
	const FName DirXName(TEXT("RootMotion_DirX"));
	const FName DirYName(TEXT("RootMotion_DirY"));
	const FName YawName(TEXT("RootMotion_Yaw"));

	bool Fail(FString* Error, const UObject* Asset, const TCHAR* Field, const FString& Reason)
	{
		if (Error) *Error = FString::Printf(TEXT("AnimationSource='%s': %s: %s"), *GetPathNameSafe(Asset), Field, *Reason);
		return false;
	}

	bool Store(double Value, float& Out, const UObject* Asset, const TCHAR* Field, FString* Error)
	{
		if (!FMath::IsFinite(Value) || FMath::Abs(Value) > TNumericLimits<float>::Max())
			return Fail(Error, Asset, Field, TEXT("computed value is non-finite or outside float range"));
		Out = static_cast<float>(Value);
		return true;
	}

	// Scaling before normalization preserves every finite nonzero authored direction, including subnormal floats.
	bool Direction(double X, double Y, FVector& Out)
	{
		const double Largest = FMath::Max(FMath::Abs(X), FMath::Abs(Y));
		if (Largest == 0.0) { Out = FVector::ZeroVector; return false; }
		X /= Largest; Y /= Largest;
		const double Length = FMath::Sqrt(X * X + Y * Y);
		Out = FVector(X / Length, Y / Length, 0.0);
		return true;
	}

	bool Scale(const FGGYGOLocomotionCurveSample& Sample, float ScaleValue,
		FGGYGOLocomotionEvaluationResult& Out, const UObject* Asset, FString* Error)
	{
		if (!FMath::IsFinite(ScaleValue) || ScaleValue < 0.0f)
			return Fail(Error, Asset, TEXT("RootMotionScale"), TEXT("must be finite and non-negative"));
		if (!Store(static_cast<double>(Sample.Speed) * ScaleValue, Out.ScaledSpeed, Asset, TEXT("ScaledSpeed"), Error)) return false;
		Out.Sample = Sample;
		Out.ScaledVelocity = Sample.Direction * Out.ScaledSpeed;
		return !Out.ScaledVelocity.ContainsNaN() || Fail(Error, Asset, TEXT("ScaledVelocity"), TEXT("must be finite"));
	}

	/** Clip coordinates are explicit and can cross multiple loops; no clock is retained. */
	bool SampleInterval(const FGGYGOLocomotionSequenceSource& Source, double Start, double End,
		double TimeRate, FGGYGOLocomotionCurveSample& Out, FString* Error)
	{
		UAnimSequence* Sequence = Source.Sequence.Get();
		FString ValidationError;
		if (!GGYGOLocomotionEvaluation::ValidateSource(Source, ValidationError))
		{ if (Error) *Error = ValidationError; return false; }
		if (!FMath::IsFinite(Start) || Start < 0.0 || !FMath::IsFinite(End) || End < Start
			|| !FMath::IsFinite(TimeRate) || TimeRate <= 0.0)
			return Fail(Error, Sequence, TEXT("Interval"), TEXT("requires finite ordered non-negative times and positive time rate"));
		const double Length = Source.PlayLength;
		const auto ReadYaw = [Sequence, Error](double Time, float& OutYaw)
		{
			const FName Names[] = {YawName};
			TArray<float> Values;
			FString ReaderError;
			if (!GGYGOAnimationSourceCurveEvaluation::EvaluateRequiredCurves(Sequence, Time, Names, Values, ReaderError))
			{ if (Error) *Error = ReaderError; return false; }
			OutYaw = Values[0];
			return true;
		};
		const auto YawAt = [&](double Time, double& Value)
		{
			float PhaseYaw = 0.0f;
			if (!Source.bLoop)
			{
				if (!ReadYaw(FMath::Min(Time, Length), PhaseYaw)) return false;
				Value = PhaseYaw;
			}
			else
			{
				const double Cycles = FMath::FloorToDouble(Time / Length);
				const double Phase = FMath::Fmod(Time, Length);
				float FirstYaw = 0.0f, LastYaw = 0.0f;
				if (!ReadYaw(0.0, FirstYaw) || !ReadYaw(Length, LastYaw) || !ReadYaw(Phase, PhaseYaw)) return false;
				Value = PhaseYaw + (static_cast<double>(LastYaw) - FirstYaw) * Cycles;
			}
			return true; // Reader failures retain their cause; the interval checks accumulated Yaw below.
		};
		const double EndPhase = Source.bLoop ? FMath::Fmod(End, Length) : FMath::Min(End, Length);
		const FName EndpointNames[] = {SpeedName, DirXName, DirYName};
		TArray<float> EndpointValues;
		FString ReaderError;
		if (!GGYGOAnimationSourceCurveEvaluation::EvaluateRequiredCurves(Sequence, EndPhase, EndpointNames, EndpointValues, ReaderError))
		{ if (Error) *Error = ReaderError; return false; }
		const float RawSpeed = EndpointValues[0];
		const float X = EndpointValues[1], Y = EndpointValues[2];
		if (!FMath::IsFinite(RawSpeed) || RawSpeed < 0.0f)
			return Fail(Error, Sequence, TEXT("RootMotion_Speed"), FString::Printf(TEXT("interval [%.9g, %.9g] has non-finite or negative endpoint speed"), Start, End));
		if (!FMath::IsFinite(X) || !FMath::IsFinite(Y))
			return Fail(Error, Sequence, TEXT("RootMotion_DirX/DirY"), TEXT("endpoint direction must be finite"));
		Out.bHasAuthoredDirection = Direction(X, Y, Out.Direction);
		if (RawSpeed > 0.0f && !Out.bHasAuthoredDirection)
			return Fail(Error, Sequence, TEXT("RootMotion_DirX/DirY"), FString::Printf(TEXT("interval [%.9g, %.9g]: positive speed requires nonzero authored direction"), Start, End));
		double StartYaw = 0.0, EndYaw = 0.0;
		if (!YawAt(Start, StartYaw) || !YawAt(End, EndYaw)) return false;
		if (!FMath::IsFinite(StartYaw) || !FMath::IsFinite(EndYaw))
			return Fail(Error, Sequence, TEXT("RootMotion_Yaw"), TEXT("interval endpoint or loop accumulation is non-finite"));
		if (!Store(static_cast<double>(RawSpeed) * TimeRate, Out.Speed, Sequence, TEXT("Speed"), Error)
			|| !Store(EndYaw, Out.YawTotalDegrees, Sequence, TEXT("YawTotalDegrees"), Error)
			|| !Store(EndYaw - StartYaw, Out.YawDeltaDegrees, Sequence, TEXT("YawDeltaDegrees"), Error)) return false;
		Out.Velocity = Out.Direction * Out.Speed;
		Out.DirectionAngle = Out.bHasAuthoredDirection ? FMath::RadiansToDegrees(FMath::Atan2(Out.Direction.Y, Out.Direction.X)) : 0.0f;
		Out.ClipLength = Source.PlayLength;
		Out.bLoopClip = Source.bLoop;
		Out.bHasCurveSource = true;
		return true;
	}
}

bool GGYGOLocomotionEvaluation::ValidateSource(const FGGYGOLocomotionSequenceSource& Source, FString& OutError)
{
	using namespace GGYGOLocomotionSourceEvaluation;
	OutError.Reset();
	UAnimSequence* Sequence = Source.Sequence.Get();
	if (!IsValid(Sequence)) return Fail(&OutError, Sequence, TEXT("Sequence"), TEXT("original route must reference a live animation"));
	if (Source.RouteKey.IsNone()) return Fail(&OutError, Sequence, TEXT("RouteKey"), TEXT("original route is required"));
	if (!FMath::IsFinite(Source.PlayLength) || Source.PlayLength <= 0.0f
		|| !FMath::IsFinite(Source.SequenceRateScale) || Source.SequenceRateScale <= 0.0f)
		return Fail(&OutError, Sequence, TEXT("PlayLength/RateScale"), TEXT("forward source requires finite positive length and rate"));
	if (Sequence->GetPlayLength() != Source.PlayLength || Sequence->RateScale != Source.SequenceRateScale)
		return Fail(&OutError, Sequence, TEXT("Configuration"), TEXT("original length or rate changed; republish configuration before evaluation"));
	for (FName Name : {SpeedName, DirXName, DirYName, YawName})
		if (!Sequence->HasCurveData(Name, false)) return Fail(&OutError, Sequence, *Name.ToString(), TEXT("required runtime curve is missing"));
	return true;
}

bool GGYGOLocomotionEvaluation::ValidateBinding(const FGGYGOLocomotionSourceBinding& Binding, FString& OutError)
{
	using namespace GGYGOLocomotionSourceEvaluation;
	OutError.Reset();
	if (Binding.Status != EGGYGOLocomotionSourceStatus::Available)
		return Fail(&OutError, Binding.WalkRunBlendSpace.Get(), TEXT("Binding"), Binding.Error.IsEmpty() ? TEXT("source publication is not available") : Binding.Error);
	for (EGGYGOLocomotionMotionType Type : {EGGYGOLocomotionMotionType::WalkStart, EGGYGOLocomotionMotionType::StartStop,
		EGGYGOLocomotionMotionType::WalkStop, EGGYGOLocomotionMotionType::RunStop, EGGYGOLocomotionMotionType::TurnBack})
	{
		const FGGYGOLocomotionSequenceSource* Source = Binding.GetSingleSource(Type);
		if (!Source) return Fail(&OutError, nullptr, TEXT("MotionRoute"), FString::Printf(TEXT("motion %d has no original animation route"), static_cast<int32>(Type)));
		if (Source->bLoop) return Fail(&OutError, Source->Sequence.Get(), TEXT("bLoop"), TEXT("single locomotion segment must not loop"));
		if (!ValidateSource(*Source, OutError)) return false;
	}
	UBlendSpace* BS = Binding.WalkRunBlendSpace.Get();
	if (!IsValid(BS) || !BS->IsA<UBlendSpace1D>() || Binding.WalkRunKey.IsNone() || !Binding.bWalkRunLoop)
		return Fail(&OutError, BS, TEXT("WalkRun"), TEXT("requires original looping BlendSpace1D route"));
	const FBlendParameter& Axis = BS->GetBlendParameter(0);
	if (Axis.Min != 0.0f || Axis.Max != 1.0f)
		return Fail(&OutError, BS, TEXT("WalkRun.Axis"), TEXT("WalkRun alpha contract requires the authored axis [0,1]"));
	const TArray<FBlendSample>& Samples = BS->GetBlendSamples();
	if (Samples.Num() == 0 || Samples.Num() != Binding.WalkRunSamples.Num())
		return Fail(&OutError, BS, TEXT("Samples"), TEXT("original sample table changed or is empty"));
	for (int32 Index = 0; Index < Samples.Num(); ++Index)
	{
		const FGGYGOLocomotionBlendSpaceSampleSource* Bound = Binding.GetWalkRunSample(Index);
		const FBlendSample& Native = Samples[Index];
		if (!Bound || Native.Animation != Bound->Source.Sequence.Get() || Native.SampleValue != Bound->SampleValue
			|| Native.RateScale != Bound->SampleRateScale || Native.bUseSingleFrameForBlending || Native.bMirror)
			return Fail(&OutError, BS, TEXT("Samples"), FString::Printf(TEXT("sample %d changed or uses unsupported single-frame/mirror semantics"), Index));
		if (!FMath::IsFinite(Bound->SampleRateScale) || Bound->SampleRateScale <= 0.0f || !Bound->Source.bLoop)
			return Fail(&OutError, BS, TEXT("Sample.RateScale/bLoop"), TEXT("loop samples require finite positive rate and loop semantics"));
		if (!ValidateSource(Bound->Source, OutError)) return false;
	}
	return true;
}

bool GGYGOLocomotionEvaluation::EvaluateSingleInterval(const FGGYGOLocomotionSequenceSource& Source,
	float StartTime, float EndTime, float RootMotionScale, FGGYGOLocomotionEvaluationResult& OutResult, FString* OutError)
{
	using namespace GGYGOLocomotionSourceEvaluation;
	OutResult = {}; if (OutError) OutError->Reset();
	FGGYGOLocomotionCurveSample Sample;
	FGGYGOLocomotionEvaluationResult Candidate;
	if (!SampleInterval(Source, static_cast<double>(StartTime) * Source.SequenceRateScale,
		static_cast<double>(EndTime) * Source.SequenceRateScale, Source.SequenceRateScale, Sample, OutError)
		|| !Scale(Sample, RootMotionScale, Candidate, Source.Sequence.Get(), OutError)) return false;
	OutResult = Candidate; return true;
}

bool GGYGOLocomotionEvaluation::EvaluateWalkRunInterval(const FGGYGOLocomotionSourceBinding& Binding,
	float StartCyclePosition, float AcceptedIntervalSeconds, float BlendAlpha, float RootMotionScale,
	FGGYGOWalkRunEvaluationResult& OutResult, FString* OutError)
{
	using namespace GGYGOLocomotionSourceEvaluation;
	OutResult = {}; if (OutError) OutError->Reset();
	FString Error;
	if (!ValidateBinding(Binding, Error)) { if (OutError) *OutError = Error; return false; }
	UBlendSpace* BS = Binding.WalkRunBlendSpace.Get();
	if (!FMath::IsFinite(StartCyclePosition) || StartCyclePosition < 0.0f
		|| !FMath::IsFinite(AcceptedIntervalSeconds) || AcceptedIntervalSeconds < 0.0f
		|| !FMath::IsFinite(BlendAlpha) || BlendAlpha < 0.0f || BlendAlpha > 1.0f)
		return Fail(OutError, BS, TEXT("WalkRun.Interval/Alpha"), TEXT("requires non-negative finite interval/cycle and alpha in [0,1]"));
	TArray<FBlendSampleData> Samples;
	int32 Triangulation = INDEX_NONE;
	if (!BS->GetSamplesFromBlendInput(FVector(BlendAlpha, 0.0, 0.0), Samples, Triangulation, false) || Samples.IsEmpty())
		return Fail(OutError, BS, TEXT("NativeSamples"), TEXT("native BlendSpace sampling returned no active samples"));
	const float Period = BS->GetAnimationLengthFromSampleData(Samples);
	if (!FMath::IsFinite(Period) || Period <= 0.0f)
		return Fail(OutError, BS, TEXT("NativePeriod"), TEXT("native BlendSpace period must be finite and positive"));
	FGGYGOWalkRunEvaluationResult Candidate;
	if (!Store(static_cast<double>(StartCyclePosition) + static_cast<double>(AcceptedIntervalSeconds) / Period,
		Candidate.EndCyclePosition, BS, TEXT("EndCyclePosition"), OutError)) return false;
	FVector Velocity = FVector::ZeroVector;
	double YawDelta = 0.0, YawTotal = 0.0;
	for (const FBlendSampleData& Native : Samples)
	{
		const FGGYGOLocomotionBlendSpaceSampleSource* Source = Binding.GetWalkRunSample(Native.SampleDataIndex);
		const float Weight = Native.GetClampedWeight();
		if (!Source || !FMath::IsFinite(Weight) || Weight < 0.0f)
			return Fail(OutError, BS, TEXT("NativeSamples"), TEXT("active sample has no original binding or finite weight"));
		FGGYGOLocomotionCurveSample Sample;
		const double Length = Source->Source.PlayLength;
		if (!SampleInterval(Source->Source, static_cast<double>(StartCyclePosition) * Length,
			static_cast<double>(Candidate.EndCyclePosition) * Length, Length / Period, Sample, OutError)) return false;
		Velocity += Sample.Velocity * Weight;
		YawDelta += static_cast<double>(Sample.YawDeltaDegrees) * Weight;
		YawTotal += static_cast<double>(Sample.YawTotalDegrees) * Weight;
	}
	FGGYGOLocomotionCurveSample Mixed;
	if (!Store(Velocity.Size(), Mixed.Speed, BS, TEXT("Mixed.Speed"), OutError)
		|| !Store(YawDelta, Mixed.YawDeltaDegrees, BS, TEXT("Mixed.YawDeltaDegrees"), OutError)
		|| !Store(YawTotal, Mixed.YawTotalDegrees, BS, TEXT("Mixed.YawTotalDegrees"), OutError)) return false;
	Mixed.Velocity = Velocity;
	Mixed.bHasAuthoredDirection = Direction(Velocity.X, Velocity.Y, Mixed.Direction);
	Mixed.DirectionAngle = Mixed.bHasAuthoredDirection ? FMath::RadiansToDegrees(FMath::Atan2(Mixed.Direction.Y, Mixed.Direction.X)) : 0.0f;
	Mixed.ClipLength = Period; Mixed.bLoopClip = true; Mixed.bHasCurveSource = true;
	// Opposed source velocities may legitimately cancel. Only individual positive-speed sources require direction.
	if (!Scale(Mixed, RootMotionScale, Candidate.Motion, BS, OutError)) return false;
	OutResult = Candidate; return true;
}

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

/**
 * @file GGYGOActionMotionEvaluation.cpp
 * @brief 原动作曲线的完整源校验与区间差分求值。
 */
#include "Character/Data/GGYGOActionMotionEvaluation.h"
#include "Character/Data/GGYGOAnimationSourceCurveEvaluation.h"

// Unity 合并会共用匿名命名空间；内部符号须限定在本职责的命名空间。
namespace GGYGOActionMotionEvaluation::Private
{
	const FName PositionNames[] = {TEXT("RootMotion_PosX"), TEXT("RootMotion_PosY"), TEXT("RootMotion_PosZ")};
	const FName YawName(TEXT("RootMotion_Yaw"));

	static bool Reject(const FGGYGOActionMotionSourceBinding& Source, const UObject* Asset,
		const TCHAR* Field, const FString& Reason, FString& Error)
	{
		Error = FString::Printf(TEXT("[Movement.ActionMotion] Montage='%s' Slot='%s' Section='%s' Source='%s' Field='%s': %s"),
			*GetPathNameSafe(Source.Montage.Get()), *Source.SlotName.ToString(), *Source.SectionName.ToString(),
			*GetPathNameSafe(Asset), Field, *Reason);
		return false;
	}
	/** Source metadata is validated by the caller in this same synchronous evaluation. */
	static bool ValidateExecutionCurves(const FGGYGOActionMotionSourceBinding& Source,
		EGGYGOMotionRotationMode& OutRotationMode, FString& OutError)
	{
		if (Source.bMontageHasRootMotion)
			return Reject(Source, Source.Montage.Get(), TEXT("NativeRootMotion"), TEXT("native animation root motion and position-curve execution are mutually exclusive"), OutError);
		TOptional<bool> HasYaw;
		for (const FGGYGOActionMotionSourceSegment& Segment : Source.Segments)
		{
			UAnimSequence* Sequence = Segment.Sequence.Get();
			if (Segment.bEnableRootMotion)
				return Reject(Source, Sequence, TEXT("NativeRootMotion"), TEXT("source sequence enables native root motion"), OutError);
			for (FName Name : PositionNames)
			{
				if (!Sequence->HasCurveData(Name, false))
					return Reject(Source, Sequence, *Name.ToString(), TEXT("required runtime cumulative-position curve is missing"), OutError);
			}
			const bool bSourceHasYaw = Sequence->HasCurveData(YawName, false);
			if (HasYaw.IsSet() && HasYaw.GetValue() != bSourceHasYaw)
				return Reject(Source, Sequence, TEXT("RootMotion_Yaw"),
					TEXT("section mixes Yaw and translation-only sources; author an explicit zero Yaw curve for stationary pieces"), OutError);
			HasYaw = bSourceHasYaw;
		}
		OutRotationMode = HasYaw.Get(false) ? EGGYGOMotionRotationMode::SourceYaw : EGGYGOMotionRotationMode::TranslationOnly;
		return true;
	}
}

bool GGYGOActionMotionEvaluation::ValidateSource(const FGGYGOActionMotionSourceBinding& Source, FString& OutError)
{
	EGGYGOMotionRotationMode Mode;
	return ValidateSource(Source, Mode, OutError);
}

bool GGYGOActionMotionEvaluation::ValidateSource(const FGGYGOActionMotionSourceBinding& Source,
	EGGYGOMotionRotationMode& OutRotationMode, FString& OutError)
{
	OutError.Reset();
	return GGYGOActionMotionSource::ValidateSourceBinding(Source, OutError)
		&& Private::ValidateExecutionCurves(Source, OutRotationMode, OutError);
}

bool GGYGOActionMotionEvaluation::EvaluateInterval(const FGGYGOActionMotionSourceBinding& Source,
	float MontageStartSeconds, float MontageEndSeconds, FVector& OutTranslation, FString& OutError)
{
	OutTranslation = FVector::ZeroVector;
	EGGYGOMotionRotationMode Mode;
	FGGYGOActionMotionEvaluationResult Result;
	if (!ValidateSource(Source, Mode, OutError)
		|| !EvaluateInterval(Source, MontageStartSeconds, MontageEndSeconds, Mode, Result, OutError)) return false;
	OutTranslation = Result.Translation;
	return true;
}

bool GGYGOActionMotionEvaluation::EvaluateInterval(const FGGYGOActionMotionSourceBinding& Source,
	float MontageStartSeconds, float MontageEndSeconds, EGGYGOMotionRotationMode OriginalRotationMode,
	FGGYGOActionMotionEvaluationResult& OutResult, FString& OutError)
{
	OutResult = {};
	OutError.Reset();
	TArray<FGGYGOActionMotionSourceInterval> Pieces;
	EGGYGOMotionRotationMode Mode;
	if (!GGYGOActionMotionSource::MapMontageInterval(Source, MontageStartSeconds, MontageEndSeconds, Pieces, OutError)
		|| !Private::ValidateExecutionCurves(Source, Mode, OutError)) return false;
	if (Mode != OriginalRotationMode)
		return Private::Reject(Source, Source.Montage.Get(), TEXT("RotationMode"), TEXT("original source Yaw capability changed during execution"), OutError);
	TArray<FName> RequiredNames;
	RequiredNames.Append(Private::PositionNames, UE_ARRAY_COUNT(Private::PositionNames));
	if (Mode == EGGYGOMotionRotationMode::SourceYaw) RequiredNames.Add(Private::YawName);
	FVector Candidate = FVector::ZeroVector;
	double YawDelta = 0.0;
	for (const FGGYGOActionMotionSourceInterval& Piece : Pieces)
	{
		if (!Source.Segments.IsValidIndex(Piece.BindingSegmentIndex))
			return Private::Reject(Source, Source.Montage.Get(), TEXT("MappedSegment"), TEXT("interval does not reference its original binding"), OutError);
		UAnimSequence* Sequence = Source.Segments[Piece.BindingSegmentIndex].Sequence.Get();
		TArray<float> FromValues, ToValues;
		if (!GGYGOAnimationSourceCurveEvaluation::EvaluateRequiredCurves(Sequence, Piece.SequenceStartSeconds, RequiredNames, FromValues, OutError)
			|| !GGYGOAnimationSourceCurveEvaluation::EvaluateRequiredCurves(Sequence, Piece.SequenceEndSeconds, RequiredNames, ToValues, OutError))
		{
			const FString ReaderError = OutError;
			return Private::Reject(Source, Sequence, TEXT("MotionCurves"), ReaderError, OutError);
		}
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const float From = FromValues[Axis], To = ToValues[Axis];
			Candidate[Axis] += static_cast<double>(To) - From;
			if (!FMath::IsFinite(Candidate[Axis]))
				return Private::Reject(Source, Sequence, TEXT("Translation"), TEXT("position interval sum is non-finite"), OutError);
		}
		if (Mode == EGGYGOMotionRotationMode::SourceYaw)
		{
			YawDelta += static_cast<double>(ToValues[3]) - FromValues[3];
			if (!FMath::IsFinite(YawDelta) || FMath::Abs(YawDelta) > TNumericLimits<float>::Max())
				return Private::Reject(Source, Sequence, TEXT("RootMotion_Yaw"), TEXT("Yaw interval sum is non-finite or outside float range"), OutError);
		}
	}
	OutResult.Translation = Candidate;
	OutResult.YawDeltaDegrees = static_cast<float>(YawDelta);
	OutResult.SourceIntervals = MoveTemp(Pieces);
	return true;
}

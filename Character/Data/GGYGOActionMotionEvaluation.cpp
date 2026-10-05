#include "Character/Data/GGYGOActionMotionEvaluation.h"
#include "Character/Data/GGYGOAnimationSourceCurveEvaluation.h"

namespace
{
	const FName PositionNames[] = {TEXT("RootMotion_PosX"), TEXT("RootMotion_PosY"), TEXT("RootMotion_PosZ")};

	bool Reject(const FGGYGOActionMotionSourceBinding& Source, const UObject* Asset,
		const TCHAR* Field, const FString& Reason, FString& Error)
	{
		Error = FString::Printf(TEXT("[Movement.ActionMotion] Montage='%s' Slot='%s' Section='%s' Source='%s' Field='%s': %s"),
			*GetPathNameSafe(Source.Montage.Get()), *Source.SlotName.ToString(), *Source.SectionName.ToString(),
			*GetPathNameSafe(Asset), Field, *Reason);
		return false;
	}
}

bool GGYGOActionMotionEvaluation::ValidateSource(const FGGYGOActionMotionSourceBinding& Source, FString& OutError)
{
	OutError.Reset();
	if (!GGYGOActionMotionSource::ValidateSourceBinding(Source, OutError)) return false;
	if (Source.bMontageHasRootMotion)
		return Reject(Source, Source.Montage.Get(), TEXT("NativeRootMotion"), TEXT("native animation root motion and position-curve execution are mutually exclusive"), OutError);
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
	}
	return true;
}

bool GGYGOActionMotionEvaluation::EvaluateInterval(const FGGYGOActionMotionSourceBinding& Source,
	float MontageStartSeconds, float MontageEndSeconds, FVector& OutTranslation, FString& OutError)
{
	OutTranslation = FVector::ZeroVector;
	OutError.Reset();
	if (!ValidateSource(Source, OutError)) return false;
	TArray<FGGYGOActionMotionSourceInterval> Pieces;
	if (!GGYGOActionMotionSource::MapMontageInterval(Source, MontageStartSeconds, MontageEndSeconds, Pieces, OutError)) return false;
	FVector Candidate = FVector::ZeroVector;
	for (const FGGYGOActionMotionSourceInterval& Piece : Pieces)
	{
		if (!Source.Segments.IsValidIndex(Piece.BindingSegmentIndex))
			return Reject(Source, Source.Montage.Get(), TEXT("MappedSegment"), TEXT("interval does not reference its original binding"), OutError);
		UAnimSequence* Sequence = Source.Segments[Piece.BindingSegmentIndex].Sequence.Get();
		TArray<float> FromValues, ToValues;
		if (!GGYGOAnimationSourceCurveEvaluation::EvaluateRequiredCurves(Sequence, Piece.SequenceStartSeconds, PositionNames, FromValues, OutError)
			|| !GGYGOAnimationSourceCurveEvaluation::EvaluateRequiredCurves(Sequence, Piece.SequenceEndSeconds, PositionNames, ToValues, OutError))
		{
			const FString ReaderError = OutError;
			return Reject(Source, Sequence, TEXT("PositionCurves"), ReaderError, OutError);
		}
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const float From = FromValues[Axis], To = ToValues[Axis];
			Candidate[Axis] += static_cast<double>(To) - From;
			if (!FMath::IsFinite(Candidate[Axis]))
				return Reject(Source, Sequence, TEXT("Translation"), TEXT("position interval sum is non-finite"), OutError);
		}
	}
	OutTranslation = Candidate;
	return true;
}

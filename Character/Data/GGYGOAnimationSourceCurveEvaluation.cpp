#include "Character/Data/GGYGOAnimationSourceCurveEvaluation.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimSequence.h"
#include "Misc/MemStack.h"

bool GGYGOAnimationSourceCurveEvaluation::EvaluateRequiredCurves(const UAnimSequence* Sequence, double SourceTime,
	TConstArrayView<FName> RequiredNames, TArray<float>& OutValues, FString& OutError)
{
	OutValues.Reset();
	OutError.Reset();
	const auto Reject = [&](FName Name, const TCHAR* Reason)
	{
		OutError = FString::Printf(TEXT("[Movement.AnimationSourceCurve] Source='%s' Curve='%s' Time=%.9g: %s"),
			*GetPathNameSafe(Sequence), *Name.ToString(), SourceTime, Reason);
		return false;
	};
	if (!IsInGameThread() || !IsValid(Sequence))
		return Reject(NAME_None, TEXT("original source must be live on the game thread"));
	const double ClipLength = Sequence->GetPlayLength();
	if (!FMath::IsFinite(ClipLength) || ClipLength <= 0.0 || !FMath::IsFinite(SourceTime)
		|| SourceTime < 0.0 || SourceTime > ClipLength || RequiredNames.IsEmpty())
		return Reject(NAME_None, TEXT("original source time must be finite and within the clip; required names cannot be empty"));
	for (FName Name : RequiredNames)
		if (Name.IsNone()) return Reject(Name, TEXT("required curve name is unset"));

	// UE5.8's scalar raw overload reads legacy storage cleared by OnModelModified(Populated).
	// The collection overload evaluates the original model or the native compressed curve codec.
	FMemMark CurveMemory(FMemStack::Get());
	FBlendedCurve Values;
	Sequence->EvaluateCurveData(Values, FAnimExtractContext(SourceTime, false), false);
	TArray<float> Candidate;
	Candidate.Reserve(RequiredNames.Num());
	for (FName Name : RequiredNames)
	{
		bool bHasValue = false;
		const float Value = Values.Get(Name, bHasValue);
		if (!bHasValue) return Reject(Name, TEXT("native evaluation omitted the required curve; no default value is admitted"));
		if (!FMath::IsFinite(Value)) return Reject(Name, TEXT("native evaluated value is non-finite"));
		Candidate.Add(Value);
	}
	OutValues = MoveTemp(Candidate);
	return true;
}

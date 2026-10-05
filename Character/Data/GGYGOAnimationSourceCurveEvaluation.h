#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

class UAnimSequence;

/** Stateless native format reader; no source routing, movement rules, clocks or retained data. */
namespace GGYGOAnimationSourceCurveEvaluation
{
	/**
	 * Read required names at one original source time through the native collection overload.
	 * The engine selects its original editor model or cooked codec; there is no alternate reader.
	 * Output follows name order, is empty on failure, and requires actual presence and finite values.
	 */
	GGYGO_API bool EvaluateRequiredCurves(const UAnimSequence* Sequence, double SourceTime,
		TConstArrayView<FName> RequiredNames, TArray<float>& OutValues, FString& OutError);
}

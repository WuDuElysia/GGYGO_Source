#include "Character/Data/GGYGOActionMotionProfile.h"
#include "Curves/CurveVector.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

bool UGGYGOActionMotionProfile::ValidateMotion(FString& OutError) const
{
	if (!TranslationCurve || !FMath::IsFinite(Duration) || Duration <= UE_SMALL_NUMBER || TranslationScale.ContainsNaN())
	{
		OutError = TEXT("Action motion requires a curve, positive finite duration and finite scale.");
		return false;
	}
	for (const FRichCurve& Axis : TranslationCurve->FloatCurves)
	{
		const TArray<FRichCurveKey>& Keys = Axis.GetConstRefOfKeys();
		if (Keys.Num() < 2 || !FMath::IsNearlyZero(Keys[0].Time, 0.0001f)
			|| !FMath::IsNearlyZero(Keys[0].Value, 0.001f)
			|| !FMath::IsNearlyEqual(Keys.Last().Time, Duration, 0.0001f))
		{
			OutError = TEXT("Every motion axis must cover [0, Duration] and start at zero displacement.");
			return false;
		}
		for (const FRichCurveKey& Key : Keys)
		{
			if (!FMath::IsFinite(Key.Time) || !FMath::IsFinite(Key.Value)
				|| !FMath::IsFinite(Key.ArriveTangent) || !FMath::IsFinite(Key.LeaveTangent))
			{
				OutError = TEXT("Action motion contains non-finite curve data.");
				return false;
			}
		}
	}
	OutError.Reset();
	return true;
}

#if WITH_EDITOR
EDataValidationResult UGGYGOActionMotionProfile::IsDataValid(FDataValidationContext& Context) const
{
	const EDataValidationResult ParentResult = Super::IsDataValid(Context);
	FString Error;
	if (!ValidateMotion(Error))
	{
		Context.AddError(FText::FromString(Error));
		return EDataValidationResult::Invalid;
	}
	return ParentResult == EDataValidationResult::Invalid ? ParentResult : EDataValidationResult::Valid;
}
#endif

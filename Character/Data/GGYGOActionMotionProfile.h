#pragma once

#include "Engine/DataAsset.h"
#include "GGYGOActionMotionProfile.generated.h"

class UCurveVector;

/** Independent ground-action trajectory. Concrete actions and timing belong to GA/assets. */
UCLASS(BlueprintType)
class GGYGO_API UGGYGOActionMotionProfile : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Cumulative displacement in imported mesh component space, centimetres, time in seconds. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Action Motion")
	TObjectPtr<UCurveVector> TranslationCurve;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Action Motion", meta = (ClampMin = "0.001", Units = "s"))
	float Duration = 0.f;

	/** Applied before the captured mesh-to-world transform. Z is excluded by the ground executor. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Action Motion")
	FVector TranslationScale = FVector::OneVector;

	bool ValidateMotion(FString& OutError) const;
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};

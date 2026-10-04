/**
 * @file GGYGOLocomotionMotionProfile.h
 * @brief Editable curve data and pure interval evaluation for locomotion motion.
 */
#pragma once

#include "CoreMinimal.h"
#include "Curves/CurveFloat.h"
#include "Character/Data/GGYGOMovementTypes.h"
#include "Engine/DataAsset.h"

#include "GGYGOLocomotionMotionProfile.generated.h"

/**
 * Authored motion curves for one locomotion clip/profile.
 *
 * The curves preserve the baked RootMotion_* semantics: speed is in cm/s,
 * DirectionX/DirectionY are UE local axes (X forward, Y right), and YawCurve
 * is cumulative degrees from the clip start. The owning Movement component
 * selects and evaluates intervals; this asset stores no playback state.
 */
UCLASS(BlueprintType)
class GGYGO_API UGGYGOLocomotionMotionProfile : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Baked RootMotion_Speed values in centimetres per second. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Locomotion Motion|Curves")
	FRuntimeFloatCurve SpeedCurve;

	/** Baked RootMotion_DirX values; positive values point forward. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Locomotion Motion|Curves")
	FRuntimeFloatCurve DirectionXCurve;

	/** Baked RootMotion_DirY values; positive values point right. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Locomotion Motion|Curves")
	FRuntimeFloatCurve DirectionYCurve;

	/** Baked RootMotion_Yaw cumulative values in degrees. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Locomotion Motion|Curves")
	FRuntimeFloatCurve YawCurve;

	/** Effective clip period in seconds. For looped clips this is the loop period. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Locomotion Motion", meta = (ClampMin = "0.001", Units = "s"))
	float Duration = 0.0f;

	/** Whether the profile repeats at Duration. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Locomotion Motion")
	bool bLoop = false;

	/** Original animation asset identifier used to bake this profile. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Locomotion Motion|Source")
	FString SourceAssetIdentifier;

	/** Stable source-content fingerprint recorded by the migration/bake pipeline. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Locomotion Motion|Source")
	FString SourceFingerprint;

	/**
	 * Evaluate one interval without modifying asset or playback state.
	 * Speed and direction are sampled at EndTime. YawTotalDegrees is the
	 * cumulative curve value at EndTime (unwrapped across loop cycles), while
	 * YawDeltaDegrees is the unwrapped difference over [StartTime, EndTime].
	 *
	 * Non-looped times are clamped to [0, Duration]. Looped times are interpreted
	 * on an unbounded non-negative timeline and wrapped for speed/direction.
	 * Invalid timing/curve data, non-finite results, negative speed, or positive
	 * speed without a valid direction fail and leave OutSample fully reset.
	 * Authored zero speed is valid. OutError is optional, cleared on entry, and
	 * receives the failing field/curve and reason; this asset does not log errors.
	 */
	bool EvaluateInterval(float StartTime, float EndTime, FGGYGOLocomotionCurveSample& OutSample,
		FString* OutError = nullptr) const;

	/** Validate curve keys and profile timing for asset authoring. */
	bool ValidateProfile(FString& OutError) const;

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};

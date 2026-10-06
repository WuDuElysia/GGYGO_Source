#pragma once

#include "CoreMinimal.h"
#include "Animation/Data/GGYGOActionMotionSourceBinding.h"

/** Pure original-position evaluation. No playback, movement permission, clock or cache. */
namespace GGYGOActionMotionEvaluation
{
	/** Original complete-range XYZ contract: all three cm curves are required; native RM is mutually exclusive. */
	GGYGO_API bool ValidateSource(const FGGYGOActionMotionSourceBinding& Source, FString& OutError);

	/**
	 * Absolute montage interval -> original per-sequence/loop cumulative-position differences, in actor-local cm.
	 * Uses native curve collection evaluation: the engine reads its original editor model or cooked codec.
	 * Every required evaluated endpoint must exist and be finite; no name-only legacy zero or alternate reader.
	 */
	GGYGO_API bool EvaluateInterval(const FGGYGOActionMotionSourceBinding& Source,
		float MontageStartSeconds, float MontageEndSeconds, FVector& OutTranslation, FString& OutError);
}

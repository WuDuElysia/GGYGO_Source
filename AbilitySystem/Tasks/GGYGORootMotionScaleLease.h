/** @file GGYGORootMotionScaleLease.h @brief Montage Task root motion scale token lease. */
#pragma once

#include "CoreMinimal.h"

class ACharacter;
class UObject;

/**
 * Small ownership helper for Montage Task root motion translation scale.
 * A later lease takes ownership while preserving the first lease's baseline;
 * only the newest token can restore that baseline.
 */
class FGGYGORootMotionScaleLease final
{
public:
	using FToken = uint64;

	static FToken Acquire(ACharacter* Character, UObject* Owner, float DesiredScale);
	static bool Release(const TWeakObjectPtr<ACharacter>& Character, const UObject* Owner, FToken Token);
};

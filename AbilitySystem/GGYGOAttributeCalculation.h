/**
 * @file GGYGOAttributeCalculation.h
 * @brief Pure comparison and channel projection of already-qualified native aggregators.
 */
#pragma once

#include "AbilitySystem/GGYGOAttributeBaseCalculationTypes.h"

struct FAggregator;
struct FAggregatorEvaluateParameters;
struct FAggregatorModChannelContainer;
enum class EGameplayModEvaluationChannel : uint8;

namespace GGYGOAttributeCalculation
{
	/** Compares frozen data only; no capture, qualification callback or current ASC access. */
	bool HasSameAttributeAggregation(const FAggregator& First, const FAggregator& Second);
	/** Projects qualified modifiers into native evaluators without GE/dependent ownership. */
	EGGYGOAttributeBaseCalculationReason PrepareNativeAttributeCalculationChannels(
		const FAggregator& QualifiedSnapshot, const FAggregatorEvaluateParameters& Parameters,
		FAggregatorModChannelContainer& OutChannels, TArray<EGameplayModEvaluationChannel>& OutOrder);
	bool IsAttributeForwardValueConfirmed(float Actual, float Desired);
}

/**
 * @file GGYGOAttributeBaseCalculationTypes.h
 * @brief Results of a stack-local native attribute base calculation.
 */
#pragma once

#include "CoreMinimal.h"

enum class EGGYGOAttributeBaseCalculationOutcome : uint8
{
	Rejected = 0,
	Ready,
	Stale
};

enum class EGGYGOAttributeBaseCalculationReason : uint8
{
	InvalidRequest = 0,
	None,
	WrongThread,
	InvalidASC,
	NotAuthority,
	InvalidAttribute,
	UnsupportedAttribute,
	MissingAttributeSet,
	SourceChanged,
	CaptureFailed,
	NonFiniteInput,
	NonFiniteSource,
	NonFiniteModifier,
	InvalidDivisor,
	NonInvertibleChannel,
	NonFiniteResult,
	ForwardMismatch,
	AggregationChanged
};

/** Stack-call calculation only. CalculatedBase is finite and usable only for Ready/None. */
struct GGYGO_API FGGYGOAttributeBaseCalculationResult
{
	EGGYGOAttributeBaseCalculationOutcome Outcome = EGGYGOAttributeBaseCalculationOutcome::Rejected;
	EGGYGOAttributeBaseCalculationReason Reason = EGGYGOAttributeBaseCalculationReason::InvalidRequest;
	float CalculatedBase = 0.0f;
};

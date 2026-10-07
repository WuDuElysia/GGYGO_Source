/**
 * @file GGYGOMovementSet.cpp
 * @brief 移动参数资产实现
 */
#include "Character/Data/GGYGOMovementSet.h"
#include "Character/Data/GGYGOLocomotionMotionProfile.h"
#include "Character/Data/GGYGOLocomotionSteeringEvaluation.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOMovementSet)

UGGYGOMovementSet::UGGYGOMovementSet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 字段默认值已在头文件声明处给出，构造函数不重复赋值，
	// 避免两处默认值不一致这种典型的维护陷阱。
}

float UGGYGOMovementSet::GetSpeedForGait(EGGYGOGait Gait) const
{
	switch (Gait)
	{
	case EGGYGOGait::Walk:
		return WalkSpeed;

	case EGGYGOGait::Run:
		return RunSpeed;

	case EGGYGOGait::None:
	default:
		return 0.0f;
	}
}

float UGGYGOMovementSet::GetSanitizedWalkToRunHoldSeconds() const
{
	// Compatibility accessor: configuration admission owns validation. Do not
	// turn an invalid value into an apparently valid walk/run policy.
	return WalkToRunHoldSeconds;
}

const UGGYGOLocomotionMotionProfile* UGGYGOMovementSet::GetProfileForMotion(EGGYGOLocomotionMotionType MotionType) const
{
	switch (MotionType)
	{
	case EGGYGOLocomotionMotionType::WalkStart:
		return WalkStartProfile;
	case EGGYGOLocomotionMotionType::StartStop:
		return StartStopProfile;
	case EGGYGOLocomotionMotionType::WalkStop:
		return WalkStopProfile;
	case EGGYGOLocomotionMotionType::RunStop:
		return RunStopProfile;
	case EGGYGOLocomotionMotionType::TurnBack:
		return TurnBackProfile;
	case EGGYGOLocomotionMotionType::WalkRun:
	case EGGYGOLocomotionMotionType::None:
	default:
		return nullptr;
	}
}

bool UGGYGOMovementSet::ValidateMovementSet(FString& OutError) const
{
	OutError.Reset();
	const auto FailField = [this, &OutError](const TCHAR* Field, const FString& Reason)
	{
		OutError = FString::Printf(TEXT("MovementSet '%s': %s: %s"), *GetPathName(), Field, *Reason);
		return false;
	};

	struct FNonNegativeRule
	{
		const TCHAR* Field;
		float Value;
	};
	const FNonNegativeRule NonNegativeRules[] = {
		{TEXT("WalkSpeed"), WalkSpeed},
		{TEXT("RunSpeed"), RunSpeed},
		{TEXT("StartStopSelectionSeconds"), StartStopSelectionSeconds},
		{TEXT("RotationYawRate"), RotationYawRate},
		{TEXT("MaxAcceleration"), MaxAcceleration},
		{TEXT("BrakingDecelerationWalking"), BrakingDecelerationWalking},
		{TEXT("GroundFriction"), GroundFriction},
		{TEXT("RootMotionScale"), RootMotionScale},
		{TEXT("TurnBackYawSettleDegrees"), TurnBackYawSettleDegrees},
		{TEXT("TurnBackRunOutMinSpeed"), TurnBackRunOutMinSpeed},
		{TEXT("TurnBackDurationSeconds"), TurnBackDurationSeconds}
	};
	for (const FNonNegativeRule& Rule : NonNegativeRules)
	{
		if (!FMath::IsFinite(Rule.Value))
		{
			return FailField(Rule.Field, TEXT("must be finite."));
		}
		if (Rule.Value < 0.0f)
		{
			return FailField(Rule.Field, TEXT("must be non-negative."));
		}
	}

	struct FRangeRule
	{
		const TCHAR* Field;
		float Value;
		float Minimum;
		float Maximum;
	};
	const FRangeRule RangeRules[] = {
		{TEXT("WalkToRunHoldSeconds"), WalkToRunHoldSeconds, 0.1f, 60.0f},
		{TEXT("WalkRunBlendInterpSpeed"), WalkRunBlendInterpSpeed, 0.0f, 50.0f},
		{TEXT("TurnBackReverseInputDotThreshold"), TurnBackReverseInputDotThreshold, -1.0f, 1.0f},
		{TEXT("TurnBackMinYawDegrees"), TurnBackMinYawDegrees, 0.0f, 180.0f},
		{TEXT("TurnBackRunOutForwardThreshold"), TurnBackRunOutForwardThreshold, 0.0f, 1.0f}
	};
	for (const FRangeRule& Rule : RangeRules)
	{
		if (!FMath::IsFinite(Rule.Value))
		{
			return FailField(Rule.Field, TEXT("must be finite."));
		}
		if (Rule.Value < Rule.Minimum || Rule.Value > Rule.Maximum)
		{
			return FailField(Rule.Field, FString::Printf(TEXT("must be in [%g, %g] (value %g)."),
				Rule.Minimum, Rule.Maximum, Rule.Value));
		}
	}

	// Motion sources are resolved from Animation's original source binding.
	// Serialized profile copies are retained only as migration history; their
	// presence or contents cannot admit (or replace) a runtime animation source.
	return GGYGOLocomotionSteeringEvaluation::ValidateConfiguration(*this, OutError);
}

#if WITH_EDITOR
EDataValidationResult UGGYGOMovementSet::IsDataValid(FDataValidationContext& Context) const
{
	const EDataValidationResult ParentResult = Super::IsDataValid(Context);
	FString Error;
	if (!ValidateMovementSet(Error))
	{
		Context.AddError(FText::FromString(Error));
		return EDataValidationResult::Invalid;
	}
	return ParentResult == EDataValidationResult::Invalid ? ParentResult : EDataValidationResult::Valid;
}
#endif

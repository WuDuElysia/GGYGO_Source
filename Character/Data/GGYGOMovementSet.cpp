/**
 * @file GGYGOMovementSet.cpp
 * @brief 移动参数资产实现
 */
#include "Character/Data/GGYGOMovementSet.h"
#include "Character/Data/GGYGOLocomotionMotionProfile.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOMovementSet)

namespace GGYGOMovementSetDefaults
{
	/** 配置值非法时的兜底走跑阈值。与字段默认值一致。 */
	constexpr float WalkToRunHoldSeconds = 1.5f;

	/** 走跑阈值上限。超过一分钟的"持续走"在任何玩法下都是配置错误。 */
	constexpr float MaxWalkToRunHoldSeconds = 60.0f;
}

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
		return FMath::Max(WalkSpeed, 0.0f);

	case EGGYGOGait::Run:
		return FMath::Max(RunSpeed, 0.0f);

	case EGGYGOGait::None:
	default:
		return 0.0f;
	}
}

float UGGYGOMovementSet::GetSanitizedWalkToRunHoldSeconds() const
{
	// 非有限值（NaN / Inf）无法参与比较，会让计时器永远达不到或立刻达到阈值。
	if (!FMath::IsFinite(WalkToRunHoldSeconds) || WalkToRunHoldSeconds <= 0.0f)
	{
		return GGYGOMovementSetDefaults::WalkToRunHoldSeconds;
	}

	return FMath::Min(WalkToRunHoldSeconds, GGYGOMovementSetDefaults::MaxWalkToRunHoldSeconds);
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

	struct FProfileRule
	{
		const TCHAR* Field;
		const UGGYGOLocomotionMotionProfile* Profile;
		bool bExpectedLoop;
	};
	const FProfileRule ProfileRules[] = {
		{TEXT("WalkStartProfile"), WalkStartProfile.Get(), false},
		{TEXT("WalkLoopProfile"), WalkLoopProfile.Get(), true},
		{TEXT("RunLoopProfile"), RunLoopProfile.Get(), true},
		{TEXT("StartStopProfile"), StartStopProfile.Get(), false},
		{TEXT("WalkStopProfile"), WalkStopProfile.Get(), false},
		{TEXT("RunStopProfile"), RunStopProfile.Get(), false},
		{TEXT("TurnBackProfile"), TurnBackProfile.Get(), false}
	};
	for (const FProfileRule& Rule : ProfileRules)
	{
		if (!Rule.Profile)
		{
			if (bUseCurveDrivenSpeed)
			{
				return FailField(Rule.Field, TEXT("a Profile is required when bUseCurveDrivenSpeed is true."));
			}
			continue;
		}
		if (!IsValid(Rule.Profile))
		{
			return FailField(Rule.Field, TEXT("references an invalid Profile object."));
		}
		if (Rule.Profile->bLoop != Rule.bExpectedLoop)
		{
			return FailField(Rule.Field, FString::Printf(TEXT("Profile '%s' must use bLoop=%s."),
				*Rule.Profile->GetPathName(), Rule.bExpectedLoop ? TEXT("true") : TEXT("false")));
		}
		FString ProfileError;
		if (!Rule.Profile->ValidateProfile(ProfileError))
		{
			return FailField(Rule.Field, FString::Printf(TEXT("Profile '%s': %s"),
				*Rule.Profile->GetPathName(), *ProfileError));
		}
	}
	return true;
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

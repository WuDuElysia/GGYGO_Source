/**
 * @file ZZZLocomotionRules.cpp
 * @brief ZZZ 动画 Locomotion 无状态纯规则实现
 */

#include "Animation/zzzAnim/Locomotion/ZZZLocomotionRules.h"
#include "Animation/zzzAnim/Data/ZZZAnimTuning.h"

namespace ZZZLocomotionRules
{
	float ResolveGaitBlendInterpSpeed(const FZZZAnimTuning* InTuning)
	{
		if (!InTuning)
		{
			return DefaultGaitBlendInterpSpeed;
		}

		const float ConfiguredSpeed = InTuning->GaitBlendInterpSpeed;
		if (!FMath::IsFinite(ConfiguredSpeed) || ConfiguredSpeed <= 0.0f)
		{
			return 0.0f;
		}

		return FMath::Min(ConfiguredSpeed, MaxGaitBlendInterpSpeed);
	}

	float ResolveTurnBackReverseInputDotThreshold(const FZZZAnimTuning* InTuning)
	{
		if (!InTuning)
		{
			return DefaultTurnBackReverseInputDotThreshold;
		}

		const float ConfiguredThreshold = InTuning->TurnBackReverseInputDotThreshold;
		if (!FMath::IsFinite(ConfiguredThreshold))
		{
			return DefaultTurnBackReverseInputDotThreshold;
		}

		return FMath::Clamp(ConfiguredThreshold, -1.0f, 0.0f);
	}

	float ResolveGaitBlendTarget(EMovementGait InSnapshotGait)
	{
		return InSnapshotGait == EMovementGait::Run ? 1.0f : 0.0f;
	}
}

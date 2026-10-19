#pragma once

#include "CoreMinimal.h"
#include "Animation/Data/GGYGOActionMotionSourceBinding.h"
#include "Character/Data/GGYGOMotionExecutionTypes.h"

/** 原累计曲线的区间差分；没有播放权、时钟或缓存。 */
struct FGGYGOActionMotionEvaluationResult
{
	FVector Translation = FVector::ZeroVector;
	float YawDeltaDegrees = 0.0f;
	TArray<FGGYGOActionMotionSourceInterval> SourceIntervals;
};

/** 原位移／Yaw 曲线纯求值，无播放权、移动许可、时钟或缓存。 */
namespace GGYGOActionMotionEvaluation
{
	/** Original complete-range XYZ contract: all three cm curves are required; native RM is mutually exclusive. */
	GGYGO_API bool ValidateSource(const FGGYGOActionMotionSourceBinding& Source, FString& OutError);
	/** 全部源均有 Yaw 或均无 Yaw；混合能力必须明确补齐，不能漏段。 */
	GGYGO_API bool ValidateSource(const FGGYGOActionMotionSourceBinding& Source,
		EGGYGOMotionRotationMode& OutRotationMode, FString& OutError);
	/** 按入场解析的原模式消费；源能力变化或必需值缺失明确失败。 */
	GGYGO_API bool EvaluateInterval(const FGGYGOActionMotionSourceBinding& Source,
		float MontageStartSeconds, float MontageEndSeconds, EGGYGOMotionRotationMode OriginalRotationMode,
		FGGYGOActionMotionEvaluationResult& OutResult, FString& OutError);

	/**
	 * Absolute montage interval -> original per-sequence/loop cumulative-position differences, in actor-local cm.
	 * Uses native curve collection evaluation: the engine reads its original editor model or cooked codec.
	 * Every required evaluated endpoint must exist and be finite; no name-only legacy zero or alternate reader.
	 */
	GGYGO_API bool EvaluateInterval(const FGGYGOActionMotionSourceBinding& Source,
		float MontageStartSeconds, float MontageEndSeconds, FVector& OutTranslation, FString& OutError);
}

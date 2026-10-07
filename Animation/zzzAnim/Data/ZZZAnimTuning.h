/**
 * @file ZZZAnimTuning.h
 * @brief ZZZ 动画状态机的运行时调参类型
 *
 * 定义运行时参数：
 *   FZZZAnimTuning        动画混合参数
 */
#pragma once

#include "CoreMinimal.h"
#include "ZZZAnimTuning.generated.h"

/** WalkRun-only presentation. Steering and gait remain owned by Movement. */
USTRUCT(BlueprintType)
struct FZZZWalkRunLeanTuning
{
	GENERATED_BODY()

	/** Explicit normal mode. Enabled configurations must supply all required values. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WalkRun Lean")
	bool bEnabled = false;

	/** Observed velocity heading yaw rate that reaches the authored maximum, in degrees/second. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WalkRun Lean", meta = (EditCondition = "bEnabled", ClampMin = "0.0"))
	float FullLeanYawRateDegreesPerSecond = 0.0f;

	/** Maximum presentation angle at WalkRun alpha 0. Bone axis/sign belong to AnimBP. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WalkRun Lean", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "90.0"))
	float WalkMaxAngleDegrees = 0.0f;

	/** Maximum presentation angle at WalkRun alpha 1; must not be lower than Walk. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WalkRun Lean", meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "90.0"))
	float RunMaxAngleDegrees = 0.0f;

	/** Positive exponential presentation response, in 1/seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WalkRun Lean", meta = (EditCondition = "bEnabled", ClampMin = "0.0"))
	float EnterResponseSpeed = 0.0f;

	/** Positive exponential recovery response, in 1/seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WalkRun Lean", meta = (EditCondition = "bEnabled", ClampMin = "0.0"))
	float RecoveryResponseSpeed = 0.0f;
};

USTRUCT(BlueprintType)
struct FZZZAnimTuning
{
	GENERATED_BODY()

	// 这里不放反向输入阈值。转身的触发判定属于移动层，阈值在
	// `UGGYGOMovementSet::TurnBackReverseInputDotThreshold`，动画层只读相位结果。
	// 两边各存一份的话，改动画层这份不会有任何效果，却看不出来。

	/** 循环动画间 BlendIn 时间（秒） */
	UPROPERTY(EditAnywhere, Category = "Blend")
	float LoopBlendIn = 0.1f;

	/** 一次性动画 BlendOut 时间（秒） */
	UPROPERTY(EditAnywhere, Category = "Blend")
	float OneShotBlendOut = 0.15f;

	/** 旧 ABP 序列化兼容字段。运行时改读 MovementSet.WalkRunBlendInterpSpeed。 */
	UPROPERTY(EditAnywhere, Category = "Deprecated", meta = (DeprecatedProperty, DeprecationMessage = "Move this value to MovementSet.WalkRunBlendInterpSpeed."))
	float GaitBlendInterpSpeed = 6.f;

	/** Character-authored WalkRun lean. Disabled is explicit; invalid enabled data fails visibly. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WalkRun Lean")
	FZZZWalkRunLeanTuning WalkRunLean;
};

/**
 * @file ZZZAnimTuning.h
 * @brief ZZZ 动画状态机的运行时调参类型
 *
 * 定义运行时参数：
 *   FZZZAnimTuning        动画混合参数
 */
#pragma once

#include "CoreMinimal.h"
#include "Curves/CurveFloat.h"
#include "ZZZAnimTuning.generated.h"

/** WalkRun-only presentation. Steering and gait remain owned by Movement. */
USTRUCT(BlueprintType)
struct FZZZWalkRunLeanTuning
{
	GENERATED_BODY()

	/** Explicit normal mode. Enabled configurations must supply all required values. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WalkRun Lean")
	bool bEnabled = false;

	/** 输入为方向偏差绝对值（0..180度），输出为偏离直立的倾身角。启用时必须明确配置；符号来自 Movement。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WalkRun Lean", meta = (EditCondition = "bEnabled"))
	FRuntimeFloatCurve DirectionErrorToLeanDegrees;

	/** 保留旧 ABP 序列化字段；当前表现只求值角色配置的响应曲线。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Deprecated", meta = (DeprecatedProperty, DeprecationMessage = "Configure DirectionErrorToLeanDegrees; the legacy linear reference is no longer consumed."))
	float FullLeanDirectionErrorDegrees = 0.0f;

	/** Serialized legacy yaw-rate tuning; never converted into direction degrees or consumed at runtime. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Deprecated", meta = (DeprecatedProperty, DeprecationMessage = "Configure DirectionErrorToLeanDegrees; yaw rate is not a direction angle."))
	float FullLeanYawRateDegreesPerSecond = 0.0f;

	/** 保留旧 ABP 序列化字段，不再缩放新响应曲线。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Deprecated", meta = (DeprecatedProperty, DeprecationMessage = "Configure DirectionErrorToLeanDegrees; the legacy Walk maximum is no longer consumed."))
	float WalkMaxAngleDegrees = 0.0f;

	/** 保留旧 ABP 序列化字段，不再缩放新响应曲线。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Deprecated", meta = (DeprecatedProperty, DeprecationMessage = "Configure DirectionErrorToLeanDegrees; the legacy Run maximum is no longer consumed."))
	float RunMaxAngleDegrees = 0.0f;

	/** Serialized legacy entry filter; valid direction deviations are now mapped directly. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Deprecated", meta = (DeprecatedProperty, DeprecationMessage = "Valid direction lean follows its authored response curve without an entry filter."))
	float EnterResponseSpeed = 0.0f;

	/** Positive recovery response in 1/seconds, only after Movement returns NotApplicable. Never filters a valid direction deviation. */
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

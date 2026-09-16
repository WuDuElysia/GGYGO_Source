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

	/** GaitBlendY 向目标值收敛的速率（1/秒）；面板钳制只约束输入，运行期仍按规则层容错规则解析 */
	UPROPERTY(EditAnywhere, Category = "Gait", meta = (ClampMin = "0.1", ClampMax = "50.0"))
	float GaitBlendInterpSpeed = 6.f;
};

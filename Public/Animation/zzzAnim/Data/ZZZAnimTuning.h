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

	/** WalkRun → TurnBack 的反向输入点积阈值；输入点积小于等于该值时视为接近反向。 */
	UPROPERTY(EditAnywhere, Category = "Locomotion", meta = (ClampMin = "-1.0", ClampMax = "0.0"))
	float TurnBackReverseInputDotThreshold = -0.95f;

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

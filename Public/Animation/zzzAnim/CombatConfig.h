/**
 * @file CombatConfig.h
 * @brief ZZZ 动画状态机的配置类型
 *
 * 定义动画配表和运行时参数：
 *   FZZZAnimSet           key→value 动画资产表（全部 key/value 由蓝图配置）
 *   FZZZAnimTuning        动画混合参数
 *
 * 代码只提供空的 map 容器 + 查询接口，
 * 具体 key 名称和对应的动画资产由美术在 AnimBP 细节面板填写。
 */
#pragma once

#include "CoreMinimal.h"
#include "CombatConfig.generated.h"

class UAnimSequence;
class UBlendSpace;

USTRUCT(BlueprintType)
struct FZZZAnimSet
{
	GENERATED_BODY()

	/** key→value 动画序列表，全部 key 和资产由蓝图配置 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Locomotion")
	TMap<FName, TObjectPtr<UAnimSequence>> Sequences;

	/** key→value BlendSpace 表，全部 key 和资产由蓝图配置 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Locomotion")
	TMap<FName, TObjectPtr<UBlendSpace>> BlendSpaces;
};

USTRUCT(BlueprintType)
struct FZZZAnimTuning
{
	GENERATED_BODY()

	/** 循环动画间 BlendIn 时间（秒） */
	UPROPERTY(EditAnywhere, Category = "Blend")
	float LoopBlendIn = 0.1f;

	/** 一次性动画 BlendOut 时间（秒） */
	UPROPERTY(EditAnywhere, Category = "Blend")
	float OneShotBlendOut = 0.15f;
};

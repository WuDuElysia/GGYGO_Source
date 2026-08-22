/**
 * @file ZZZAnimSet.h
 * @brief ZZZ 动画层的动画资产集合
 *
 * 定义由蓝图配置的 Locomotion 动画资产映射。
 */
#pragma once

#include "CoreMinimal.h"
#include "ZZZAnimSet.generated.h"

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

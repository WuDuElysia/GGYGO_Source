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

	// Explicit semantic routes into the existing tables. These initial values preserve the
	// established ZZZ table keys; a missing key/asset never selects a substitute route.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Locomotion|Source Routes")
	FName WalkStartSourceKey = FName(TEXT("WalkStart"));

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Locomotion|Source Routes")
	FName StartStopSourceKey = FName(TEXT("WalkStartEnd"));

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Locomotion|Source Routes")
	FName WalkStopSourceKey = FName(TEXT("WalkEnd"));

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Locomotion|Source Routes")
	FName RunStopSourceKey = FName(TEXT("RunEnd"));

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Locomotion|Source Routes")
	FName TurnBackSourceKey = FName(TEXT("TurnBack"));

	/** Loop sequences are resolved from this actual BlendSpace's samples, not a second asset table. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Locomotion|Source Routes")
	FName WalkRunSourceKey = FName(TEXT("walkRun"));
};

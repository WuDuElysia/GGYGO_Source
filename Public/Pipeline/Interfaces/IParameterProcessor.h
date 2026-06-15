/**
 * @file IParameterProcessor.h
 * @brief 参数处理器接口
 *
 * 读取 RuntimeData 意图字段 → 写入 RuntimeData 动画参数字段。
 * 所有参数处理器必须实现此接口。
 */
#pragma once

#include "CoreMinimal.h"

struct FRuntimeData;

class IParameterProcessor
{
public:
	virtual ~IParameterProcessor() = default;

	/**
	 * 每帧处理参数
	 * @param RuntimeData 运行时黑板（读意图、写参数）
	 * @param DeltaTime   帧间隔
	 */
	virtual void Process(FRuntimeData& RuntimeData, float DeltaTime) = 0;
};

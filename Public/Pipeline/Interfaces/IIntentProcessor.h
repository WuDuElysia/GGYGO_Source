/**
 * @file IIntentProcessor.h
 * @brief 意图处理器接口
 *
 * 读取 InputData（由 InputPipeline 处理后的数据）→ 写入 RuntimeData 意图字段。
 * 所有意图处理器必须实现此接口。
 */
#pragma once

#include "CoreMinimal.h"

class FInputData;
struct FRuntimeData;

class IIntentProcessor
{
public:
	virtual ~IIntentProcessor() = default;

	/**
	 * 每帧处理意图
	 * @param InputData  输入数据（只读，由 InputPipeline 写入）
	 * @param RuntimeData 运行时黑板（写入意图字段）
	 */
	virtual void Process(const FInputData& InputData, FRuntimeData& RuntimeData) = 0;
};

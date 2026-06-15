/**
 * @file IArbiter.h
 * @brief 仲裁器接口
 *
 * 每个仲裁器在 Tick 最开头执行，读取系统状态，写入 RuntimeData 仲裁标记。
 * 仲裁器之间不互相依赖，通过 RuntimeData 间接通信。
 */
#pragma once

#include "CoreMinimal.h"

struct FRuntimeData;

class IArbiter
{
public:
	virtual ~IArbiter() = default;

	/**
	 * 每帧仲裁
	 * @param RuntimeData 运行时黑板（读取状态、写入仲裁标记）
	 * @param DeltaTime   帧间隔
	 */
	virtual void Arbitrate(FRuntimeData& RuntimeData, float DeltaTime) = 0;
};

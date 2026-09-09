/**
 * @file LocomotionIntentProcessor.h
 * @brief 移动方向意图处理器
 *
 * 仅负责将输入方向转换为世界空间的 RuntimeData.Intent.DesiredWorldMoveDir，并同步
 * RuntimeData.ZZZAnim.bShouldMove；步态解析由 Gait_Authority 独立负责。
 */
#pragma once

#include "Pipeline/Interfaces/IIntentProcessor.h"

class FLocomotionIntentProcessor : public IIntentProcessor
{
public:
	virtual void Process(const FInputData& InputData, FRuntimeData& RuntimeData) override;
};

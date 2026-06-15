/**
 * @file LocomotionIntentProcessor.h
 * @brief 移动意图处理器
 *
 * 摇杆/WASD 输入 → 结合 ControlRotation → 世界空间方向 DesiredWorldMoveDir。
 * 依赖 ViewRotationProcessor 先写入 ControlRotation。
 */
#pragma once

#include "Pipeline/Interfaces/IIntentProcessor.h"

class FLocomotionIntentProcessor : public IIntentProcessor
{
public:
	virtual void Process(const FInputData& InputData, FRuntimeData& RuntimeData) override;
};

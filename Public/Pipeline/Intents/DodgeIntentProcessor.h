/**
 * @file DodgeIntentProcessor.h
 * @brief 闪避意图处理器
 *
 * 闪避键缓冲（由 InputPipeline 维护）→ RuntimeData.Intent.bWantsToDodge。
 */
#pragma once

#include "Pipeline/Interfaces/IIntentProcessor.h"

class FDodgeIntentProcessor : public IIntentProcessor
{
public:
	virtual void Process(const FInputData& InputData, FRuntimeData& RuntimeData) override;
};

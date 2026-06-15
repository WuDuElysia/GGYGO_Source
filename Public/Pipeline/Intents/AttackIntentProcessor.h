/**
 * @file AttackIntentProcessor.h
 * @brief 攻击意图处理器
 *
 * 攻击键缓冲（由 InputPipeline 维护）→ bWantsToAttack。
 */
#pragma once

#include "Pipeline/Interfaces/IIntentProcessor.h"

class FAttackIntentProcessor : public IIntentProcessor
{
public:
	virtual void Process(const FInputData& InputData, FRuntimeData& RuntimeData) override;
};

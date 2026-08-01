/**
 * @file AttackIntentProcessor.cpp
 * @brief 攻击意图处理器实现
 */
#include "Pipeline/Intents/AttackIntentProcessor.h"
#include "Data/InputData.h"
#include "Data/Logic/RuntimeData.h"

void FAttackIntentProcessor::Process(const FInputData& InputData, FRuntimeData& RuntimeData)
{
	if (InputData.CurrentFrame.IsAttackPressed())
	{
		RuntimeData.bWantsToAttack = true;
	}
}

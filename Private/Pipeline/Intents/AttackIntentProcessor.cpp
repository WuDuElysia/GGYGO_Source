/**
 * @file AttackIntentProcessor.cpp
 * @brief 攻击意图处理器实现
 */
#include "Pipeline/Intents/AttackIntentProcessor.h"
#include "Data/Input/InputData.h"
#include "Data/Runtime/RuntimeData.h"

void FAttackIntentProcessor::Process(const FInputData& InputData, FRuntimeData& RuntimeData)
{
	if (InputData.CurrentFrame.IsAttackPressed())
	{
		RuntimeData.Intent.bWantsToAttack = true;
	}
}

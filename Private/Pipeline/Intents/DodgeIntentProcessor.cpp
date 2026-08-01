/**
 * @file DodgeIntentProcessor.cpp
 * @brief 闪避意图处理器实现
 */
#include "Pipeline/Intents/DodgeIntentProcessor.h"
#include "Data/InputData.h"
#include "Data/Logic/RuntimeData.h"

void FDodgeIntentProcessor::Process(const FInputData& InputData, FRuntimeData& RuntimeData)
{
	if (InputData.CurrentFrame.IsDodgePressed())
	{
		RuntimeData.bWantsToDodge = true;
	}
}

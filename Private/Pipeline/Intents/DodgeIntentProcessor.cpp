/**
 * @file DodgeIntentProcessor.cpp
 * @brief 闪避意图处理器实现
 */
#include "Pipeline/Intents/DodgeIntentProcessor.h"
#include "Data/Input/InputData.h"
#include "Data/Runtime/RuntimeData.h"

void FDodgeIntentProcessor::Process(const FInputData& InputData, FRuntimeData& RuntimeData)
{
	if (InputData.CurrentFrame.IsDodgePressed())
	{
		RuntimeData.Intent.bWantsToDodge = true;
	}
}

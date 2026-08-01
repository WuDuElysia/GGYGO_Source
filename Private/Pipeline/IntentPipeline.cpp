/**
 * @file IntentPipeline.cpp
 * @brief 意图管线实现
 */
#include "Pipeline/IntentPipeline.h"
#include "Pipeline/Intents/ViewRotationProcessor.h"
#include "Pipeline/Intents/LocomotionIntentProcessor.h"
#include "Pipeline/Intents/AttackIntentProcessor.h"
#include "Pipeline/Intents/DodgeIntentProcessor.h"
#include "Pipeline/Parameters/MovementParameterProcessor.h"
#include "Pipeline/Parameters/RootMotionParameterProcessor.h"


void FIntentPipeline::Init(ACharacter* InOwner, USkeletalMeshComponent* InMesh)
{
	// 创建意图处理器（顺序固定，ViewRotation 必须在 Locomotion 之前）
	auto ViewRotProc = MakeUnique<FViewRotationProcessor>();
	ViewRotProc->Init(InOwner);
	IntentProcessors.Add(MoveTemp(ViewRotProc));

	IntentProcessors.Add(MakeUnique<FLocomotionIntentProcessor>());
	IntentProcessors.Add(MakeUnique<FAttackIntentProcessor>());
	IntentProcessors.Add(MakeUnique<FDodgeIntentProcessor>());

	// 创建参数处理器（顺序固定）
	ParameterProcessors.Add(MakeUnique<FMovementParameterProcessor>());

	auto RootMotionProc = MakeUnique<FRootMotionParameterProcessor>();
	RootMotionProc->Init(InMesh);
	ParameterProcessors.Add(MoveTemp(RootMotionProc));
}

void FIntentPipeline::ProcessIntents(const FInputData& InputData, FRuntimeData& RuntimeData)
{
	for (const auto& Processor : IntentProcessors)
	{
		Processor->Process(InputData, RuntimeData);
	}
}

void FIntentPipeline::ProcessParameters(FRuntimeData& RuntimeData, float DeltaTime)
{
	for (const auto& Processor : ParameterProcessors)
	{
		Processor->Process(RuntimeData, DeltaTime);
	}
}

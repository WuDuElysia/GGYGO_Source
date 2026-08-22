/**
 * @file IntentPipeline.cpp
 * @brief 意图管线实现
 */
#include "Pipeline/IntentPipeline.h"
#include "CoreGlobals.h"
#include "Pipeline/Gait/GaitLog.h"
#include "Pipeline/Intents/ViewRotationProcessor.h"
#include "Pipeline/Intents/LocomotionIntentProcessor.h"
#include "Pipeline/Intents/AttackIntentProcessor.h"
#include "Pipeline/Intents/DodgeIntentProcessor.h"
#include "Pipeline/Parameters/MovementParameterProcessor.h"
#include "Pipeline/Parameters/RootMotionParameterProcessor.h"
#include "Pipeline/Parameters/AnimSignalParameterProcessor.h"
#include "Pipeline/Parameters/TurnBackPhaseProcessor.h"


void FIntentPipeline::Init(ACharacter* InOwner, USkeletalMeshComponent* InMesh)
{
	// 步态决策者是具名阶段成员，不加入意图/参数处理器数组。
	GaitAuthority.Init(InOwner);

	// 创建意图处理器（顺序固定，ViewRotation 必须在 Locomotion 之前）
	auto ViewRotProc = MakeUnique<FViewRotationProcessor>();
	ViewRotProc->Init(InOwner);
	IntentProcessors.Add(MoveTemp(ViewRotProc));

	IntentProcessors.Add(MakeUnique<FLocomotionIntentProcessor>());
	IntentProcessors.Add(MakeUnique<FAttackIntentProcessor>());
	IntentProcessors.Add(MakeUnique<FDodgeIntentProcessor>());

	// 创建参数处理器（顺序固定）
	auto MovementProc = MakeUnique<FMovementParameterProcessor>();
	MovementProc->Init(InOwner);
	ParameterProcessors.Add(MoveTemp(MovementProc));

	auto RootMotionProc = MakeUnique<FRootMotionParameterProcessor>();
	RootMotionProc->Init(InMesh);
	ParameterProcessors.Add(MoveTemp(RootMotionProc));

	// 通用动画信号采样必须在 TurnBack 相位之前，保证相位读到本帧最新的 sig_turnback。
	auto AnimSignalProc = MakeUnique<FAnimSignalParameterProcessor>();
	AnimSignalProc->Init(InMesh);
	ParameterProcessors.Add(MoveTemp(AnimSignalProc));

	// TurnBack 相位机：读取本帧输入方向、步态和动画信号，写 RuntimeData.Movement.TurnBack.Phase。
	auto TurnBackPhaseProc = MakeUnique<FTurnBackPhaseProcessor>();
	TurnBackPhaseProc->Init(InOwner);
	ParameterProcessors.Add(MoveTemp(TurnBackPhaseProc));
}

void FIntentPipeline::ProcessIntents(const FInputData& InputData, FRuntimeData& RuntimeData)
{
	for (const auto& Processor : IntentProcessors)
	{
		Processor->Process(InputData, RuntimeData);
	}
}

void FIntentPipeline::ProcessGait(const FInputData& InputData, FRuntimeData& RuntimeData, float DeltaTime)
{
	GaitAuthority.Process(InputData, RuntimeData, DeltaTime);
	LastGaitFrameCounter = GFrameCounter;
}

void FIntentPipeline::ProcessParameters(FRuntimeData& RuntimeData, float DeltaTime)
{
	// Gait 阶段必须在参数阶段入口前完成；缺失帧不补写，步态状态自然保持上一帧。
	if (LastGaitFrameCounter != GFrameCounter)
	{
		if (!bGaitStageMissReported)
		{
			UE_LOG(LogGait, Warning,
				TEXT("Gait stage was not executed for frame %llu before ProcessParameters; gait state was left unchanged."),
				GFrameCounter);
			bGaitStageMissReported = true;
		}
	}
	else
	{
		bGaitStageMissReported = false;
	}

	for (const auto& Processor : ParameterProcessors)
	{
		Processor->Process(RuntimeData, DeltaTime);
	}
}

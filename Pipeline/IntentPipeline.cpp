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

	// TurnBack 相位机：读取本帧输入方向、步态和 DeltaTime，
	// 从角色 MovementConfig 的时间轴写 RuntimeData.Movement.TurnBack。
	auto TurnBackPhaseProc = MakeUnique<FTurnBackPhaseProcessor>();
	TurnBackPhaseProc->Init(InOwner);
	TurnBackPhaseProcessor = TurnBackPhaseProc.Get();
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

void FIntentPipeline::NotifyCanYaw()
{
	// AnimInstance 回调不直接写 RuntimeData；TurnBack 参数处理器在下一次参数阶段消费事件闩。
	if (TurnBackPhaseProcessor)
	{
		TurnBackPhaseProcessor->NotifyCanYaw();
	}
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

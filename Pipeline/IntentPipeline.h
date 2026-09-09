/**
 * @file IntentPipeline.h
 * @brief 意图管线 - 持有所有处理器，提供统一执行入口
 *
 * 在 BaseCharacter::Tick 中按序调用（紧跟在 InputPipeline 之后）。
 * 处理器执行顺序固定，不提供运行时重排接口。
 */
#pragma once

#include "CoreMinimal.h"
#include "Pipeline/Gait/GaitAuthorityProcessor.h"
#include "Pipeline/Interfaces/IIntentProcessor.h"
#include "Pipeline/Interfaces/IParameterProcessor.h"

class ACharacter;
class USkeletalMeshComponent;
class FInputData;
class FTurnBackPhaseProcessor;
struct FRuntimeData;

class FIntentPipeline
{
public:
	/**
	 * 初始化所有处理器，注入外部依赖
	 * @param InOwner 角色指针（ViewRotationProcessor 需要）
	 * @param InMesh  骨骼网格体（RootMotionParameterProcessor 需要）
	 */
	void Init(ACharacter* InOwner, USkeletalMeshComponent* InMesh);

	/**
	 * 执行所有意图处理器（Tick 第 3 步）
	 * 前置条件: InputPipeline.Process() 已执行
	 */
	void ProcessIntents(const FInputData& InputData, FRuntimeData& RuntimeData);

	/**
	 * 执行步态决策阶段（Tick 第 3.5 步）
	 * 前置条件: ProcessIntents() 已执行；必须早于 ProcessParameters()、MotionDriver 与动画驱动
	 */
	void ProcessGait(const FInputData& InputData, FRuntimeData& RuntimeData, float DeltaTime);

	/**
	 * 执行所有参数处理器（Tick 第 4 步）
	 * 前置条件: ProcessIntents() 已执行
	 */
	void ProcessParameters(FRuntimeData& RuntimeData, float DeltaTime);

	/**
	 * 动画 CanYaw Notify 的事件边界；事件只转发到 TurnBackPhaseProcessor 的待消费闩，
	 * RuntimeData 的 bCanYaw/bSecondSegment 仍由参数阶段统一写入。
	 */
	void NotifyCanYaw();

private:
	/** 意图处理器列表（按执行顺序） */
	TArray<TUniquePtr<IIntentProcessor>> IntentProcessors;

	/** 参数处理器列表（按执行顺序） */
	TArray<TUniquePtr<IParameterProcessor>> ParameterProcessors;

	/** TurnBack 参数处理器的非拥有引用，用于接收动画 CanYaw Notify。 */
	FTurnBackPhaseProcessor* TurnBackPhaseProcessor = nullptr;

	/** 步态决策者：具名成员，不加入任何处理器数组。 */
	FGaitAuthorityProcessor GaitAuthority;

	/** 最近一次成功执行步态阶段的引擎帧号。 */
	uint64 LastGaitFrameCounter = 0;

	/** 看门狗诊断节流：步态阶段恢复执行后复位。 */
	bool bGaitStageMissReported = false;
};

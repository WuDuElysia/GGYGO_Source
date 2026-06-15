/**
 * @file IntentPipeline.h
 * @brief 意图管线 - 持有所有处理器，提供统一执行入口
 *
 * 在 BaseCharacter::Tick 中按序调用（紧跟在 InputPipeline 之后）。
 * 处理器执行顺序固定，不提供运行时重排接口。
 */
#pragma once

#include "CoreMinimal.h"
#include "Pipeline/Interfaces/IIntentProcessor.h"
#include "Pipeline/Interfaces/IParameterProcessor.h"

class ACharacter;
class USkeletalMeshComponent;
class FInputData;
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
	 * 执行所有参数处理器（Tick 第 4 步）
	 * 前置条件: ProcessIntents() 已执行
	 */
	void ProcessParameters(FRuntimeData& RuntimeData, float DeltaTime);

private:
	/** 意图处理器列表（按执行顺序） */
	TArray<TUniquePtr<IIntentProcessor>> IntentProcessors;

	/** 参数处理器列表（按执行顺序） */
	TArray<TUniquePtr<IParameterProcessor>> ParameterProcessors;
};

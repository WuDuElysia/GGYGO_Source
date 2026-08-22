/**
 * @file AnimSignalParameterProcessor.cpp
 * @brief 通用动画信号采样处理器实现
 */
#include "Pipeline/Parameters/AnimSignalParameterProcessor.h"
#include "Data/Runtime/RuntimeData.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"

const FName& GGYGOAnimSignals::TurnBack()
{
	// 函数内静态，确保 FName 在名字子系统就绪后才构造，避免全局静态初始化顺序问题。
	static const FName Name(TEXT("sig_turnback"));
	return Name;
}

void FAnimSignalParameterProcessor::Init(USkeletalMeshComponent* InMesh)
{
	Mesh = InMesh;

	// 已知动画信号的名字表：新增信号在这里追加一行即可，无需改采样逻辑或消费方。
	SignalCurveNames.Reset();
	SignalCurveNames.Add(GGYGOAnimSignals::TurnBack());
}

void FAnimSignalParameterProcessor::Process(FRuntimeData& RuntimeData, float /*DeltaTime*/)
{
	// 每帧重置信号表；缺失曲线自然缺省为 0（消费方 GetAnimSignal 返回 0）。
	RuntimeData.AnimSignals.Reset();

	if (!Mesh)
	{
		return;
	}

	const UAnimInstance* AnimInstance = Mesh->GetAnimInstance();
	if (!AnimInstance)
	{
		return;
	}

	for (const FName& SignalName : SignalCurveNames)
	{
		const float Value = AnimInstance->GetCurveValue(SignalName);
		RuntimeData.AnimSignals.Add(SignalName, FMath::IsFinite(Value) ? Value : 0.f);
	}
}

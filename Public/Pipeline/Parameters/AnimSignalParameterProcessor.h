/**
 * @file AnimSignalParameterProcessor.h
 * @brief 通用动画信号采样处理器
 *
 * 每帧按一张配置好的曲线名字表，从当前 Mesh 的 UAnimInstance 采样对应曲线，
 * 写入 RuntimeData.AnimSignals（Key=曲线名，Value=采样值）。
 *
 * 设计目的：把"动画→逻辑"的信号统一成可每帧查询的数据，避免为每个信号
 * 单独写采样点或散落的 if。新增信号只需在名字表里加一行；消费方用
 * RuntimeData::GetAnimSignal(Name) 按名字读取，缺失即视为 0。
 */
#pragma once

#include "CoreMinimal.h"
#include "Pipeline/Interfaces/IParameterProcessor.h"

class USkeletalMeshComponent;

/** 集中登记动画信号曲线名，供采样器与各消费方共用同一真相，避免字符串漂移。 */
namespace GGYGOAnimSignals
{
	/** TurnBack 解冻信号曲线名（0/1 阶梯，>=0.5 表示可解冻并跟随输入）。 */
	const FName& TurnBack();
}

class FAnimSignalParameterProcessor : public IParameterProcessor
{
public:
	/**
	 * @param InMesh 采样曲线的骨骼网格体（其 AnimInstance 提供曲线值）
	 * 采样名字表默认登记全部已知信号；新增信号在此追加一行即可。
	 */
	void Init(USkeletalMeshComponent* InMesh);

	virtual void Process(FRuntimeData& RuntimeData, float DeltaTime) override;

private:
	USkeletalMeshComponent* Mesh = nullptr;

	/** 需要每帧采样的信号曲线名字表；新增信号只需在 Init 中追加。 */
	TArray<FName> SignalCurveNames;
};

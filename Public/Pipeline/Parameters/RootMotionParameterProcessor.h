/**
 * @file RootMotionParameterProcessor.h
 * @brief 根运动数据提取处理器
 *
 * 从 GGYGOAnimInstance 读取当前动画的根骨骼位移（Root Motion Delta）。
 * 不再依赖自定义 "Speed" 曲线，直接使用 AnimSequence 内置的根骨骼运动数据。
 *
 * 数据流（新版）：
 *   AnimSequence 内置 RM 数据 → GGYGOGOAnimInstance 每帧提取 → RootMotionDelta
 *   → 本处理器读取 → RuntimeData.AnimSpeed / RootMotionDelta
 *   → MotionDriver 用实际位移向量驱动移动（防滑步）
 */
#pragma once

#include "Pipeline/Interfaces/IParameterProcessor.h"

class USkeletalMeshComponent;

class FRootMotionParameterProcessor : public IParameterProcessor
{
public:
	void Init(USkeletalMeshComponent* InMesh);

	virtual void Process(FRuntimeData& RuntimeData, float DeltaTime) override;

private:
	USkeletalMeshComponent* Mesh = nullptr;
};

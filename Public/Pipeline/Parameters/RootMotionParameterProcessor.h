/**
 * @file RootMotionParameterProcessor.h
 * @brief 五根动画曲线驱动参数处理器
 *
 * 从当前 Mesh 的 UAnimInstance 读取 RM_PosX、RM_PosY、RM_Dist、RM_Speed、RM_Yaw。
 * 位置、距离和 yaw 曲线是累计采样值，处理器跨帧差分后把局部位移、
 * yaw 增量和诊断数据写入 RuntimeData；RM_Speed 是唯一动画速度主值，
 * AnimSpeed 仅保留为该曲线安全采样值的外部兼容镜像，不是速度回退来源。
 * RM_Yaw 只保留为源动画旋转诊断和姿态抵消输入；sig_turnback 解冻时的 Actor 目标方向由
 * RootMotionDelta 与目标输入方向的一致性选择直接或反向方向，无有效候选时再回退输入/入口方向。
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
	/** 清除跨动画/跨 Mesh 的累计曲线基线。 */
	void ResetSampleState();

	USkeletalMeshComponent* Mesh = nullptr;

	/** 上一帧累计位置/距离/yaw 曲线采样值。 */
	float PreviousPosX = 0.f;
	float PreviousPosY = 0.f;
	float PreviousDist = 0.f;
	float PreviousYaw = 0.f;
	bool bHasPreviousSample = false;
};

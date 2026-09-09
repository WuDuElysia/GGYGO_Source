/**
 * @file RootMotionParameterProcessor.h
 * @brief 动画曲线驱动参数处理器
 *
 * 从当前 Mesh 的 UAnimInstance 读取位置、距离、速度、RM_Yaw 和 authored 速度方向曲线。RM_PosX/RM_PosY
 * 保留原始曲线分量（X=左右、Y=前后），处理器跨帧差分后写出原始分量位移和方向；MotionDriver 再将其转换为
 * UE 局部 X=前、Y=右。RM_Speed 是唯一动画速度主值，RM_Yaw 是以度为单位的累计角度曲线，处理器用相邻
 * 采样值差分输出当前帧角度增量；RM_VelocityDirX/Y 有效时提供 authored 方向，否则回退到位置差分。
 * RootMotionDelta 参与曲线方向和位移路径，TurnBack Phase/第二段仍由逻辑层提供。
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

	/** 上一帧累计位置、RM_Dist 和 RM_Yaw 样本；PreviousDist 用于检测曲线回退，PreviousYaw 用于计算相邻采样差分。 */
	float PreviousPosX = 0.f;
	float PreviousPosY = 0.f;
	float PreviousDist = 0.f;
	float PreviousYaw = 0.f;

	/** 是否已经建立有效采样基线；false 时当前采样只建立 PreviousYaw/位置/距离基线。 */
	bool bHasPreviousSample = false;
};

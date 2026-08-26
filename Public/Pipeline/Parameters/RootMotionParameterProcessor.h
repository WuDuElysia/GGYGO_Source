/**
 * @file RootMotionParameterProcessor.h
 * @brief 动画曲线驱动参数处理器
 *
 * 从当前 Mesh 的 UAnimInstance 读取位置、距离、速度、RM_Yaw 和 authored 速度方向曲线。RM_PosX/RM_PosY
 * 是固定动画起始根轨迹坐标中的累计位置，处理器跨帧差分后写出固定坐标位移和方向；RM_Speed 是唯一
 * 动画速度主值，RM_Yaw 是累计角度曲线，处理器用相邻采样值差分输出当前帧角度增量；
 * RM_VelocityDirX/Y 有效时提供同一坐标系的 authored 方向，
 * 否则回退到位置差分。RootMotionDelta 参与曲线方向和位移路径，TurnBack Phase/第二段仍由逻辑层提供。
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

	/** 上一帧累计位置、距离和 RM_Yaw 曲线采样值。 */
	float PreviousPosX = 0.f;
	float PreviousPosY = 0.f;
	float PreviousDist = 0.f;
	float PreviousYaw = 0.f;
	bool bHasPreviousSample = false;
};

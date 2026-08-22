/**
 * @file RootMotionRuntimeModel.h
 * @brief Root Motion 曲线采样输出模型
 */
#pragma once

#include "CoreMinimal.h"

/** RootMotionParameterProcessor 写入、MotionDriver 读取的本帧曲线数据。 */
struct FRootMotionRuntimeModel
{
	/** RM_Speed 安全采样值，是当前移动速度主值。 */
	float AnimCurveSpeed = 0.f;

	/** RM_Speed 的外部兼容镜像。 */
	float AnimSpeed = 0.f;

	/** RM_Dist 本帧差分，仅用于诊断。 */
	float AnimCurveDistanceDelta = 0.f;

	/** RM_PosX/RM_PosY 差分推导的局部方向角。 */
	float AnimCurveAngle = 0.f;

	/** RM_Yaw 累计源旋转采样值。 */
	float AnimCurveYaw = 0.f;

	/** RM_Yaw 本帧角度增量。 */
	float AnimCurveYawDelta = 0.f;

	/** RM_PosX/RM_PosY 的本帧局部位移差分。 */
	FVector RootMotionDelta = FVector::ZeroVector;

	/** 本帧是否存在有效的曲线位置差分。 */
	bool bHasRootMotion = false;
};

/**
 * @file RootMotionRuntimeModel.h
 * @brief Root Motion 曲线采样输出模型
 *
 * RM_PosX/RM_PosY 与 RM_VelocityDirX/Y 使用固定动画起始根轨迹坐标（X=前、Y=右）；
 * 该模型由参数处理器写入，由 MotionDriver 消费为位移速度、方向和 D0 yaw 增量；模型本身不直接写 Actor rotation。
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

	/** RM_Yaw 累计曲线当前采样值与上一帧采样值的差分（度）；TurnBack d0 由 MotionDriver 直接累加到 Actor yaw。 */
	float AnimCurveYawDelta = 0.f;

	/** RM_Dist 本帧差分，仅用于诊断。 */
	float AnimCurveDistanceDelta = 0.f;

	/** RM_PosX/RM_PosY 差分得到的固定动画曲线坐标速度（cm/s）。 */
	FVector AnimCurveVelocity = FVector::ZeroVector;

	/** 最终有效的固定动画曲线坐标方向；优先来自 RM_VelocityDirX/Y，缺失或无效时回退到 RM_PosX/RM_PosY 差分。 */
	FVector AnimCurveVelocityDirection = FVector::ZeroVector;

	/** 当前帧方向是否来自资产 authored RM_VelocityDirX/Y，而不是 RM_PosX/RM_PosY 差分回退。 */
	bool bHasAuthoredVelocityDirection = false;

	/** 最终有效方向在固定动画曲线坐标中的方向角（度）。 */
	float AnimCurveAngle = 0.f;

	/** 当前帧是否存在有效 RootMotion 曲线源（速度、方向或距离任一有效）。 */
	bool bHasRootMotionCurveSource = false;

	/** RM_PosX/RM_PosY 的本帧局部位移差分。 */
	FVector RootMotionDelta = FVector::ZeroVector;

	/** 本帧是否存在有效的曲线位置差分。 */
	bool bHasRootMotion = false;
};

/**
 * @file RootMotionRuntimeModel.h
 * @brief Root Motion 曲线采样输出模型
 *
 * RM_PosX/RM_PosY 与 RM_VelocityDirX/Y 保留原始曲线分量（X=左右、Y=前后）；
 * MotionDriver 再将其转换为 Bone_Root/UE 局部坐标（X=前、Y=右）后映射到世界。
 * 该模型由参数处理器写入；MotionDriver 消费速度主值、方向、位移和 D0 yaw 增量，
 * ZZZAnim Snapshot 读取动画曲线速度投影；模型本身不直接写 Actor rotation。
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

	/** RM_Yaw 累计曲线当前采样值与上一帧采样值的差分（度）；首帧/重建基线帧为 0，D0 由 MotionDriver 应用到 Actor yaw。 */
	float AnimCurveYawDelta = 0.f;

	/** RM_PosX/RM_PosY 差分得到的原始曲线分量速度（cm/s，X=左右、Y=前后）。 */
	FVector AnimCurveVelocity = FVector::ZeroVector;

	/** 最终有效方向的原始曲线分量（X=左右、Y=前后）；MotionDriver 转换为 UE 局部 X=前、Y=右。 */
	FVector AnimCurveVelocityDirection = FVector::ZeroVector;

	/** 当前帧方向是否来自资产 authored RM_VelocityDirX/Y，而不是 RM_PosX/RM_PosY 差分回退。 */
	bool bHasAuthoredVelocityDirection = false;

	/** 最终有效方向在原始曲线分量中的方向角（度）；MotionDriver 映射后用于 Bone_Root 世界方向计算。 */
	float AnimCurveAngle = 0.f;

	/** 当前帧是否存在有效 RootMotion 曲线源（速度、方向或距离任一有效）。 */
	bool bHasRootMotionCurveSource = false;

	/** RM_PosX/RM_PosY 的本帧原始曲线分量位移差分（X=左右、Y=前后）；有效时参与曲线方向和位移路径。 */
	FVector RootMotionDelta = FVector::ZeroVector;

	/** 本帧是否存在有效的曲线位置差分。 */
	bool bHasRootMotion = false;
};

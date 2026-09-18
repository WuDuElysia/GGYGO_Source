/**
 * @file GGYGOAnimationDebugFrame.h
 * @brief 不属于稳定 AnimBP 契约的动画诊断数据
 */
#pragma once

#include "CoreMinimal.h"

/**
 * 兼容旧 ZZZ 调试面板所需的数据。
 *
 * 本结构不暴露给蓝图，也不参与权威判断。旧字段清理完成后可以整体删除，
 * 不会污染 FGGYGOAnimationStateFrame 的稳定语义契约。
 */
struct FGGYGOAnimationDebugFrame
{
	FVector CurveVelocity = FVector::ZeroVector;
	FVector CurveVelocityDirection = FVector::ZeroVector;
	float CurveVelocityAngle = 0.0f;
	float InputForwardDot = 1.0f;
};

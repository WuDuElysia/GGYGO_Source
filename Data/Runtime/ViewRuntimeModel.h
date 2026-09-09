/**
 * @file ViewRuntimeModel.h
 * @brief 角色视角运行时模型
 */
#pragma once

#include "CoreMinimal.h"

/** ViewRotationProcessor 输出的本帧控制器旋转快照。 */
struct FViewRuntimeModel
{
	/** 控制器最终旋转；移动方向转换主要使用 Yaw。 */
	FRotator ControlRotation = FRotator::ZeroRotator;
};

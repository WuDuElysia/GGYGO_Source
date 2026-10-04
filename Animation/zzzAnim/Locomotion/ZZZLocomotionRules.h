/**
 * @file ZZZLocomotionRules.h
 * @brief ZZZ 动画 Locomotion 的无状态纯规则
 *
 * 本命名空间只承载 Movement 停止语义到既有 AnimBP 分支索引的纯映射。
 */
#pragma once

#include "CoreMinimal.h"
#include "Character/Data/GGYGOMovementTypes.h"

namespace ZZZLocomotionRules
{
	/** 映射到 ABP_Pyrios 当前 Stop Select：Start=0、Walk=1、Run=2，None 使用安全默认 0。 */
	int32 ResolveStopValue(EGGYGOStopMotionType InStopMotionType);
}

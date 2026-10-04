/**
 * @file ZZZLocomotionRules.cpp
 * @brief ZZZ 动画 Locomotion 无状态纯规则实现
 */

#include "Animation/zzzAnim/Locomotion/ZZZLocomotionRules.h"

namespace ZZZLocomotionRules
{
	int32 ResolveStopValue(EGGYGOStopMotionType InStopMotionType)
	{
		switch (InStopMotionType)
		{
		case EGGYGOStopMotionType::StartStop:
			return 0;
		case EGGYGOStopMotionType::WalkStop:
			return 1;
		case EGGYGOStopMotionType::RunStop:
			return 2;
		case EGGYGOStopMotionType::None:
		default:
			return 0;
		}
	}
}

/**
 * @file ZZZLocomotionEvents.cpp
 * @brief 旧 AnimBP Locomotion 字段的兼容映射实现
 */

#include "Animation/zzzAnim/Locomotion/ZZZLocomotionEvents.h"
#include "Animation/zzzAnim/Data/ZZZAnimSnapshot.h"
#include "Animation/zzzAnim/Locomotion/ZZZLocomotionRules.h"

void FZZZLocomotionEvents::Reset()
{
	Context = {};
}

void FZZZLocomotionEvents::SetContext(const FZZZAnimWriteContext& InContext)
{
	Context = InContext;
}

void FZZZLocomotionEvents::MapMovementState()
{
	if (!Context.Snap || !Context.Memory)
	{
		return;
	}

	// GaitBlendY 仅保留为已序列化蓝图存储；当前值直接来自 Movement，不在动画层插值。
	Context.Memory->GaitBlendY = Context.Snap->WalkRunBlendAlpha;
	Context.Memory->StopValue = ZZZLocomotionRules::ResolveStopValue(Context.Snap->StopMotionType);
}

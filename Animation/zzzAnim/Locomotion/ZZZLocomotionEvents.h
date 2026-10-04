/**
 * @file ZZZLocomotionEvents.h
 * @brief 旧 AnimBP Locomotion 字段的只读语义兼容映射
 *
 * Movement 是走跑混合与停止选择的权威来源。本类只把快照值映射到现有蓝图存储。
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/zzzAnim/Data/ZZZAnimContext.h"
#include "Animation/zzzAnim/Data/ZZZAnimStateMemory.h"

/** 把 Movement 快照映射到 ABP_Pyrios 已有字段，不维护跨帧规则或计时。 */
class FZZZLocomotionEvents
{
public:
	/** 清理不拥有的当前帧指针上下文；可重复调用。 */
	void Reset();

	/** 绑定当前帧的可写动画上下文。 */
	void SetContext(const FZZZAnimWriteContext& InContext);

	/** 复制 Movement 权威走跑混合值，并把 StopMotionType 映射为旧 Select 索引。 */
	void MapMovementState();

private:
	/** 当前帧可写动画上下文，不拥有所指对象的生命周期。 */
	FZZZAnimWriteContext Context;
};

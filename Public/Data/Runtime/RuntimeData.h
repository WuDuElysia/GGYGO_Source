/**
 * @file RuntimeData.h
 * @brief 逻辑层运行时上下文：各领域 model 的聚合入口
 *
 * FRuntimeData 保留为管线之间的运行时上下文，但不再直接承载所有领域字段。
 * 各子系统通过对应的 model 交换数据；处理器仍通过本类型保持现有接口契约。
 * 帧级意图在帧末由 ResetFrameIntents() 清零。
 */
#pragma once

#include "CoreMinimal.h"
#include "Contracts/Animation/ZZZAnimRuntimeModel.h" // FZZZAnimRuntimeModel：ZZZ 游戏线程投影
#include "Contracts/Animation/AnimSignalFrame.h"
#include "Data/Runtime/ArbiterRuntimeModel.h"
#include "Data/Runtime/GaitRuntimeModel.h"
#include "Data/Runtime/IntentRuntimeModel.h"
#include "Data/Runtime/MovementRuntimeModel.h"
#include "Data/Runtime/RootMotionRuntimeModel.h"
#include "Data/Runtime/StateRuntimeModel.h"
#include "Data/Runtime/ViewRuntimeModel.h"

/**
 * 运行时上下文 — 管线阶段之间的聚合数据入口。
 *
 * 逻辑字段是各领域 model 的 canonical 数据；ZZZAnim 只作为逻辑到新版
 * ZZZ 动画层的游戏线程投影，随后由 ZZZAnimSnapshotCapture 复制到快照。
 */
struct FRuntimeData
{
	FIntentRuntimeModel Intent;
	FViewRuntimeModel View;
	FGaitRuntimeModel Gait;
	FMovementRuntimeModel Movement;
	FArbiterRuntimeModel Arbiter;
	FStateRuntimeModel State;
	FRootMotionRuntimeModel RootMotion;
	FAnimSignalFrame AnimSignals;

	/** 新版 ZZZ 动画层的游戏线程运行时投影。 */
	FZZZAnimRuntimeModel ZZZAnim;

	/** 兼容旧调用方的动画信号读取入口。 */
	float GetAnimSignal(FName SignalName) const
	{
		return AnimSignals.Get(SignalName);
	}

	/** 帧末只清理攻击/闪避意图；跨帧 model 状态不在此处清零。 */
	void ResetFrameIntents()
	{
		Intent.ResetFrameIntents();
	}
};

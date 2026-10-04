/**
 * @file ZZZAnimStateMemory.h
 * @brief ZZZ 动画状态机的跨帧记忆
 *
 * 与 FZZZAnimSnapshot 的区别：
 *   Snap    每帧由管线重建，表示"本帧输入与逻辑现状"
 *   Memory  跨帧持久，表示"动画状态机自己记住的事"
 *
 * 状态机状态不在此重复记录；AnimBP 可用原生节点查询当前状态，Gait 与
 * TurnBackPhase 由 AnimationStateFrame 直接发布。这里只保留真正跨帧的表现记忆。
 */
#pragma once

#include "CoreMinimal.h"
#include "ZZZAnimStateMemory.generated.h"

/**
 * 动画状态机保留的蓝图兼容存储。
 * 走跑混合值与停止语义来自 Movement；本结构仅维持现有 ABP 序列化字段。
 */
USTRUCT(BlueprintType)
struct FZZZAnimStateMemory
{
	GENERATED_BODY()

	/**
	 * WalkRun BlendSpace1D 的 X 轴混合权重：0 = Walk，1 = Run。
	 * GaitBlendY 仅作为已有蓝图序列化兼容存储；显示名为 Walk Run Blend Alpha。
	 * 由动画兼容映射逐帧写入，不代表动画层持有走跑状态。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Locomotion", meta = (DisplayName = "Walk Run Blend Alpha"))
	float GaitBlendY = 0.f;

	/**
	 * Stop Select 的既有动画分支索引：0 = WalkStartEnd，1 = WalkEnd，2 = RunEnd。
	 * Movement 的 StopMotionType 由动画兼容层映射到此字段。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
	int32 StopValue = 0;

};

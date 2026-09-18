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
 * 动画状态机需要跨帧保留的最小记忆。
 * GaitBlendY 由 FZZZLocomotionEvents 写入，供 WalkRun BlendSpace 的平滑表现使用；
 * StopValue 是 Stop Select 的独立动画分支索引，不属于移动步态。
 */
USTRUCT(BlueprintType)
struct FZZZAnimStateMemory
{
	GENERATED_BODY()

	/**
	 * WalkRun BlendSpace 的 Y 轴输入，取值域为 [0, 1]；
	 * 唯一写入方：FZZZLocomotionEvents。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
	float GaitBlendY = 0.f;

	/**
	 * Stop Select 的独立动画分支索引，不属于移动步态；只允许 0~2：
	 * 0 = WalkStartEnd，1 = WalkEnd，2 = RunEnd。起步早停时的 0.5 秒窗口由
	 * FZZZLocomotionEvents 在 C++ 中每帧维护，窗口内优先写入 0；该字段不是步态。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
	int32 StopValue = 0;

};

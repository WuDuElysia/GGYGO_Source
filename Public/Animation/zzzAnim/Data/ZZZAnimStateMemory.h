/**
 * @file ZZZAnimStateMemory.h
 * @brief ZZZ 动画状态机的跨帧记忆
 *
 * 与 FZZZAnimSnapshot 的区别：
 *   Snap    每帧由管线重建，表示"本帧输入与逻辑现状"
 *   Memory  跨帧持久，表示"动画状态机自己记住的事"
 *
 * 当前顶层动画状态不在此记录 —— AnimBP 可用原生节点查询
 * （GetCurrentStateName / GetInstanceCurrentStateTime 等）；Moving 子状态、TurnBack 相位
 * 和 Locomotion 的步态/停止表现记忆保留在此结构中。
 * GaitValue 记录最近一次有效 Moving 步态；StopValue 是 Stop Select 的独立动画
 * 分支索引，不属于移动步态。
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/zzzAnim/Data/ZZZAnimEnums.h"
#include "ZZZAnimStateMemory.generated.h"

/**
 * 动画状态机需要跨帧保留的最小记忆。
 * GaitBlendY 由 FZZZLocomotionEvents 写入，供 WalkRun BlendSpace 的平滑表现使用；
 * GaitValue 记录最近一次有效 Moving 步态；StopValue 是 Stop Select 的独立动画
 * 分支索引，不属于移动步态。步态取值与当前逻辑状态均由 FZZZAnimSnapshot 提供。
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
	 * 最近一次有效 Moving 步态的离散记录：初始为 0，Walk 为 1，Run 为 2；
	 * 供需要离散步态信息的 AnimBP 读取。Snapshot Gait 为 None/非法、离开 Moving
	 * 或上下文停止推进时不清零已有的有效值；非法存储值由事件层规范为 0。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
	int32 GaitValue = 0;

	/**
	 * Stop Select 的独立动画分支索引，不属于移动步态；只允许 0~2：
	 * 0 = WalkStartEnd，1 = WalkEnd，2 = RunEnd。起步早停时的 0.5 秒窗口由
	 * FZZZLocomotionEvents 在 C++ 中每帧维护，窗口内优先写入 0；该字段不是步态。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
	int32 StopValue = 0;

	/**
	 * 当前 Moving 子状态；由 C++ pipeline 从 RuntimeData.Movement.TurnBack.Phase 投影同步，供 AnimBP 读取。
	 * None=未进入，WalkRun=普通走跑，TurnBack=转身中（Frozen 或 Released）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
	EZZZAnimMovingSubState MovingSubState = EZZZAnimMovingSubState::None;

};

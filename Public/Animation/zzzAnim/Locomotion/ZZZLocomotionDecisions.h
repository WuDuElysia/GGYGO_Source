/**
 * @file ZZZLocomotionDecisions.h
 * @brief ZZZ 动画 Locomotion 决策模块
 *
 * 只读三个上下文，不访问 Actor、RuntimeData 或 UZZZAnimInstance：
 *   Snap    每帧由管线重建，表示"本帧输入与逻辑现状"
 *   Memory  跨帧持久，表示"动画状态机自己记住的事"
 *   Tuning  运行期只读配置，不拥有其生命周期
 *
 * 本类不写任何状态。TurnBack 记忆与返回资格由事件层在 C++ 中维护，
 * 过渡条件只读该记忆；求值次数和时机都不影响状态同步。
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/zzzAnim/Data/ZZZAnimContext.h"

/**
 * ZZZ Locomotion 过渡决策类。
 *
 * 判定集合包含 NotMoving_To_Conduit()、Stop_To_Conduit()、
 * Conduit_To_Moving_Direct()、Conduit_To_EnterMove()、ShouldStopMoving()，以及
 * Moving 内的 WalkRun_To_TurnBack()；WalkRun → TurnBack 的检测只读逻辑相位。
 * Back → WalkRun 的完整动画播放条件由 AnimBP 自己判断，不再由本类提供。
 * ShouldStopMoving() 是 Moving → Stop 与 EnterMove → Stop 两个 Blueprint 入口
 * 共同使用的唯一底层停止输入判定。
 * UZZZAnimInstance 负责注入上下文，并将底层结果转发给 AnimBP 的过渡入口。
 * 所有决策函数为 const 且只读，线程安全。
 */
class FZZZLocomotionDecisions
{
public:
	/**
	 * 每次快照更新后由 UZZZAnimInstance 注入决策上下文。
	 * @param InContext 本帧只读动画上下文
	 */
	void SetContext(const FZZZAnimReadContext& InContext);

	/** NotMoving → Conduit：有移动输入且逻辑状态已经是 Moving。 */
	bool NotMoving_To_Conduit() const;

	/**
	 * Stop → Conduit：重新启动移动入口，仅依据本帧移动输入/意图。
	 * 上下文缺失时返回 false；不读取速度、Gait 或 StateMemory。
	 */
	bool Stop_To_Conduit() const;

	/**
	 * Conduit → EnterMove：本帧 Snapshot_Gait 不是 Run，走起步路径。
	 *
	 * 与 Conduit_To_Moving_Direct() 互补；上下文缺失时确定性地落到起步路径。
	 */
	bool Conduit_To_EnterMove() const;

	/** Conduit → Moving：本帧 Snapshot_Gait 已是 Run，走直接进入路径。 */
	bool Conduit_To_Moving_Direct() const;

	/**
	 * EnterMove → Stop 的基础停止输入判定。
	 * 仅当本帧没有移动输入（Snapshot.bShouldMove == false）时返回 true；不是速度为零判断，
	 * 上下文缺失时返回 false，避免快照无效时误触发停止。
	 */
	bool ShouldStopMoving() const;

	/**
	 * Moving → Stop 的停止输入判定。
	 * 与 ShouldStopMoving 相同地检查无输入，但 TurnBack 处于任一非 None 阶段时保持 false，
	 * 以保证逻辑时间轴完成前不会因中途松开输入而提前退出顶层 Moving。
	 */
	bool ShouldExitMoving() const;

	/** WalkRun → TurnBack：当前是 Moving、步态为 Run 且有输入，并且输入接近角色当前前向的反方向。 */
	bool WalkRun_To_TurnBack() const;

private:
	/** 本帧只读动画上下文，不拥有所指对象的生命周期。 */
	FZZZAnimReadContext Context;
};

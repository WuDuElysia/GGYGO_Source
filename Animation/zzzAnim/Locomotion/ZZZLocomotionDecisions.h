/**
 * @file ZZZLocomotionDecisions.h
 * @brief ZZZ 动画 Locomotion 决策模块
 *
 * 只读三个上下文，不访问 Actor 或 UZZZAnimInstance：
 *   Snap    每帧由管线重建，表示"本帧输入与逻辑现状"
 *   Memory  跨帧持久，表示"动画状态机自己记住的事"
 *   Tuning  运行期只读配置，不拥有其生命周期
 *
 * 本类不写任何状态 —— 写入集中在 `FZZZLocomotionEvents`。
 * 这个分工的意义在于：AnimBP 的过渡条件可能被求值任意多次（甚至在 worker 线程上），
 * 如果判定顺手改了状态，求值次数就会影响结果。
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/zzzAnim/Data/ZZZAnimContext.h"

/**
 * ZZZ Locomotion 过渡决策类。
 *
 * 七个判定，对应 AnimBP 里七条需要逻辑条件的过渡：
 * `NotMoving_To_Conduit`、`Stop_To_Conduit`、`Conduit_To_EnterMove`、
 * `Conduit_To_Moving_Direct`、`ShouldStopMoving`（EnterMove → Stop）、
 * `ShouldExitMoving`（Moving → Stop）、`WalkRun_To_TurnBack`。
 *
 * 纯动画时序的过渡不在这里：EnterMove → Moving、TurnBack → WalkRun、
 * Stop → NotMoving 都由 AnimBP 用 `Time Remaining (ratio)` 自己判定。
 *
 * 所有函数 const 且只读，线程安全，求值次数与时机都不影响状态。
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
	 * 与 ShouldStopMoving 相同地检查无输入，但 TurnBack 处于任一非 None 相位时保持 false，
	 * 以保证转身走完前不会因中途松开输入而提前退出顶层 Moving。
	 */
	bool ShouldExitMoving() const;

	/**
	 * WalkRun → TurnBack：移动层的转身相位处于曲线接管段（`Turning` / `Braking`）。
	 *
	 * 反向输入的几何判定在移动层做，这里只读相位结果。
	 * 刻意**排除** `RunOut`：那一段方向已交回输入、与普通走跑无异，
	 * 且若把它算进来，AnimBP 靠动画播完切回 WalkRun 之后会立刻重新进入 TurnBack，
	 * 造成转身动画循环播放。
	 */
	bool WalkRun_To_TurnBack() const;

private:
	/** 本帧只读动画上下文，不拥有所指对象的生命周期。 */
	FZZZAnimReadContext Context;
};

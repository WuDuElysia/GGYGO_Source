/**
 * @file ZZZLocomotionEvents.h
 * @brief ZZZ 动画 Locomotion 状态事件模块
 *
 * 每帧推进 `FZZZAnimStateMemory` 里真正需要跨帧的表现参数。
 * 写入集中在一处，「这个值是谁改的」才有唯一答案。
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/zzzAnim/Data/ZZZAnimContext.h"
#include "Animation/zzzAnim/Data/ZZZAnimStateMemory.h"
#include "Animation/zzzAnim/Locomotion/ZZZLocomotionRules.h"

/**
 * ZZZ Locomotion 状态事件类。
 *
 * 通过可写上下文访问快照、配置与跨帧记忆，维护两个值：
 *
 * | 值 | 含义 | AnimBP 里的去处 |
 * |---|---|---|
 * | `GaitBlendY` | 走跑连续混合 0..1 | WalkRun 状态的 BlendSpace **X 轴**输入 |
 * | `StopValue` | 刹停动画分支索引 | Stop 状态的 Select 索引 |
 *
 * `StopValue` 是动画分支索引而不是步态：0 = `WalkStartEnd`（起步早停窗口内）、
 * 1 = `WalkEnd`、2 = `RunEnd`。窗口时长是
 * `ZZZLocomotionRules::EnterMoveStopWindowSeconds`，由本类每帧计时，
 * 不由 AnimBP 或步态字段推导。
 */
class FZZZLocomotionEvents
{
public:
	/** 绑定当前帧的可写动画上下文。 */
	void SetContext(const FZZZAnimWriteContext& InContext);

	/**
	 * 每帧推进：先维护 StopValue 与 EnterMove 窗口，
	 * 再推进 GaitBlendY 向目标收敛。
	 */
	void AdvanceGaitBlend(float DeltaSeconds);

private:
	/**
	 * 维护 StopValue、EnterMove 起步早停窗口与相关计时。
	 * 必须在 GaitBlendY 的本帧更新之前执行。
	 */
	void AdvanceStopSelection(float DeltaSeconds);

	/** Snapshot_Gait 为 None 时沿用上一帧目标，否则解析当前快照目标值。 */
	float ResolveTargetFromSnapshot() const;

	/** 对非法 Snapshot_Gait 输出驻留级诊断，不改写快照源值。 */
	void ReportInvalidSnapshotGaitIfNeeded();

	/** 按恒定速率向目标推进 GaitBlendY，不越过目标值。 */
	void AdvanceGaitBlendY(float Target, float ClampedDelta);

	/** 当前帧可写动画上下文，不拥有所指对象的生命周期。 */
	FZZZAnimWriteContext Context;

	/** 上一帧解析出的 GaitBlendY 目标值；Snapshot_Gait 为 None 时沿用它继续收敛。 */
	float LastGaitBlendTarget = 0.0f;

	/**
	 * EnterMove 起步早停窗口的累计时间（秒）。由 AdvanceStopSelection 推进，
	 * 单帧增量按 `ZZZLocomotionRules::MaxClampedDelta` 钳制 —— 卡顿帧的巨大
	 * DeltaTime 会一帧跨过整个窗口，让「刚起步就松手」拿不到 WalkStartEnd。
	 */
	float EnterMoveElapsedSeconds = 0.0f;

	/** EnterMove 起步早停窗口是否仍有效；有效时 StopValue 优先固定为 0。 */
	bool bEnterMoveWindowActive = false;

	/** 上一帧快照是否处于逻辑 Moving，用于识别本次进入 Moving 的边沿。 */
	bool bWasMoving = false;

	/** 无效帧间隔诊断的驻留级节流标记。 */
	bool bInvalidDeltaReported = false;

	/** 非法 Snapshot_Gait 诊断的驻留级节流标记。 */
	bool bInvalidSnapshotGaitReported = false;
};

/**
 * @file ZZZLocomotionEvents.h
 * @brief ZZZ 动画 Locomotion 状态事件模块
 *
 * C++ pipeline 每帧驱动 Locomotion 的离散选择与表现推进，具体记忆更新
 * 都封装在本类中。AnimBP 不再通过顶层状态进入事件初始化 Stop 或 Gait。
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/zzzAnim/Data/ZZZAnimContext.h"
#include "Animation/zzzAnim/Data/ZZZAnimStateMemory.h"
#include "Animation/zzzAnim/Locomotion/ZZZLocomotionRules.h"

/**
 * ZZZ Locomotion 状态事件类。
 *
 * 事件层通过可写上下文访问快照、配置和跨帧记忆，
 * 由 C++ pipeline 维护离散 GaitValue、StopValue、EnterMove 起步早停窗口，
 * 并推进 GaitBlendY 与 Moving 子状态记录。StopValue 是 Stop Select 的独立动画
 * 分支索引，不属于移动步态；0.5 秒窗口由 C++ 每帧计时，不由 AnimBP 或步态字段推导。
 */
class FZZZLocomotionEvents
{
public:
	/** 绑定当前帧的可写动画上下文。 */
	void SetContext(const FZZZAnimWriteContext& InContext);

	/**
	 * 由 C++ 根据当前快照唯一同步 Moving 子状态；AnimBP 不再写入子状态。
	 * 该同步把 Frozen/Released 投影为 TurnBack；Back → WalkRun 的动画播放完成由 AnimBP 自己判断。
	 */
	void SynchronizeMovingSubState();

	/**
	 * 每帧由 C++ pipeline 驱动：先独立维护 StopValue 与 EnterMove 窗口，
	 * 再按 Snapshot_Gait 同步有效的 GaitValue，并推进 GaitBlendY 向对应目标值收敛。
	 */
	void AdvanceGaitBlend(float DeltaSeconds);

private:
	/**
	 * 独立维护 StopValue、EnterMove 0.5 秒窗口和相关计时状态。
	 * 必须在 GaitValue 与 GaitBlendY 的本帧更新之前执行。
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
	 * EnterMove 起步早停窗口的累计时间（秒）。由 AdvanceStopSelection 使用有效的非负 DeltaSeconds
	 * 推进，并按 ZZZLocomotionRules::MaxClampedDelta 限制单帧增量。
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

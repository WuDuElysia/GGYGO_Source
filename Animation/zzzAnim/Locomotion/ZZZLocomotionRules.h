/**
 * @file ZZZLocomotionRules.h
 * @brief ZZZ 动画 Locomotion 的无状态纯规则
 *
 * 本命名空间集中承载配置解析与值判定规则。所有函数均不持有上下文、
 * 不产生副作用，并且不改写传入配置的存储值。
 */
#pragma once

#include "CoreMinimal.h"
#include "StateMachine/CharacterStateType.h" // EMovementGait

struct FZZZAnimTuning;

namespace ZZZLocomotionRules
{
	/** GaitBlendY 插值速率默认值（1/秒）。 */
	inline constexpr float DefaultGaitBlendInterpSpeed = 2.0f;

	/** GaitBlendY 插值速率上限（1/秒）。 */
	inline constexpr float MaxGaitBlendInterpSpeed = 50.0f;

	/** WalkRun → TurnBack 的默认反向输入点积阈值。 */
	inline constexpr float DefaultTurnBackReverseInputDotThreshold = -0.95f;

	/** 单帧推进的帧间隔上界（秒）。 */
	inline constexpr float MaxClampedDelta = 0.1f;

	/**
	 * EnterMove 起步早停窗口时长（秒）。
	 * 该窗口由 C++ 事件层每帧计时；窗口内 StopValue 固定为 0，表示 WalkStartEnd。
	 */
	inline constexpr float EnterMoveStopWindowSeconds = 0.3f;

	/** GaitBlendY 吸附目标的容差。 */
	inline constexpr float GaitBlendSnapTolerance = 0.001f;

	/**
	 * 解析生效的 GaitBlendY 插值速率。
	 * 配置为空返回默认值；配置非有限或小于等于 0 时返回 0，否则上钳到上限。
	 */
	float ResolveGaitBlendInterpSpeed(const FZZZAnimTuning* InTuning);

	/** 解析 WalkRun → TurnBack 的反向输入点积阈值，并限制到 [-1, 0]。 */
	float ResolveTurnBackReverseInputDotThreshold(const FZZZAnimTuning* InTuning);

	/**
	 * 由 Snapshot_Gait 推导 GaitBlendY 目标值：仅 Run 为 1，其余取值为 0。
	 * 正向判定同时为非法 Snapshot_Gait 提供按 Walk 处理的容错，不改写源值。
	 */
	float ResolveGaitBlendTarget(EMovementGait InSnapshotGait);
}

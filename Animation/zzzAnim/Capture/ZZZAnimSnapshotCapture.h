/**
 * @file ZZZAnimSnapshotCapture.h
 * @brief 把通用动画语义帧适配成旧 ZZZ 快照
 *
 * 迁移期间保留旧 Snapshot / Decisions / Events 给现有 AnimBP 使用。
 * 本类型不再访问 Actor、CMC 或 ASC；跨层读取已经收敛到
 * `FGGYGOAnimationStateCapture`。
 */
#pragma once

#include "CoreMinimal.h"

struct FGGYGOAnimationDebugFrame;
struct FGGYGOAnimationStateFrame;
struct FZZZAnimSnapshot;

/** 兼容快照适配器。无状态，可安全地作为 AnimInstance 的成员反复调用。 */
class FZZZAnimSnapshotCapture
{
public:
	/**
	 * 从已抓取的通用帧生成旧快照。
	 *
	 * @param OutSnap 输出；函数开头会整体重置，不保留上一帧残留。
	 * @param InState 稳定的动画语义事实。
	 * @param InDebug 只用于兼容旧调试字段的数据，不参与状态判定。
	 */
	void Capture(
		FZZZAnimSnapshot& OutSnap,
		const FGGYGOAnimationStateFrame& InState,
		const FGGYGOAnimationDebugFrame& InDebug) const;
};

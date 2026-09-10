/**
 * @file ZZZAnimSnapshotCapture.h
 * @brief 每帧把移动层状态抓成一份动画快照
 *
 * 这是动画层与移动层之间的**唯一**读取点。所有跨层取值都收敛在这里，
 * 于是"动画依赖了移动层的什么"这个问题只需要看一个文件。
 *
 * 阶段 5 起数据源从 `FRuntimeData`（随 Pipeline 退役）改为
 * `UGGYGOCharacterMovementComponent`。
 */
#pragma once

#include "CoreMinimal.h"

class ACharacter;
struct FZZZAnimSnapshot;

/** 快照抓取器。无状态，可安全地作为 AnimInstance 的成员反复调用。 */
class FZZZAnimSnapshotCapture
{
public:
	/**
	 * 抓取一帧。
	 *
	 * @param OutSnap 输出。**函数开头会整体重置**，不保留上一帧任何残留 ——
	 *                部分更新会让"某字段本帧没有生产者"表现为沿用旧值，
	 *                那种错误极难定位。
	 * @param InOwner 角色。为空、或身上没有项目 CMC 时，快照保持全默认值。
	 *                后者是有意的容错：旧 `ABaseCharacter` 用的是引擎原生 CMC，
	 *                迁移期间两种角色会共存，不该因此崩掉。
	 */
	void Capture(FZZZAnimSnapshot& OutSnap, const ACharacter* InOwner) const;
};

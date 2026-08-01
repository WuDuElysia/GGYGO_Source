/**
 * @file ZZZLocomotionDecisions.h
 * @brief ZZZ 动画 Locomotion 决策模块
 *
 * 主要依赖 FZZZAnimSnapshot 做决策，不访问 Actor、RuntimeData
 * 或 UZZZAnimInstance；一次性冲刺分流开关只在对应过渡成立后消费。
 */
#pragma once

#include "CoreMinimal.h"

struct FZZZAnimSnapshot;

/**
 * ZZZ Locomotion 过渡决策类。
 *
 * UZZZAnimInstance 只负责把当前快照转发给本类，并将结果暴露给 AnimBP。
 * 所有决策函数保持 const，便于在动画求值阶段安全读取。
 */
class FZZZLocomotionDecisions
{
public:
	/** 每次快照更新后由 UZZZAnimInstance 绑定当前快照。 */
	void SetSnap(FZZZAnimSnapshot* InSnap);

	/** NotMoving → Conduit：有移动输入且逻辑状态已经是 Moving。 */
	bool NotMoving_To_Conduit() const;

	/** Conduit → EnterMove：当前帧没有冲刺触发。 */
	bool Conduit_To_EnterMove() const;

	/** Conduit → Moving（Sprint）：有冲刺触发，成功后消费本帧开关。 */
	bool Conduit_To_Moving_Sprint() const;

private:
	/** 当前帧动画快照，决策类只持有指针，不拥有其生命周期。 */
	FZZZAnimSnapshot* Snap = nullptr;
};

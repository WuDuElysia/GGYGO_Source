/**
 * @file GGYGOMovementTypes.h
 * @brief 移动层的公共枚举
 *
 * 单独成文件是因为这些类型同时被移动层（CMC）与表现层（AnimInstance）引用，
 * 放在任一侧都会让另一侧被迫 include 一整套不需要的东西。
 */
#pragma once

#include "CoreMinimal.h"

#include "GGYGOMovementTypes.generated.h"

/**
 * 步态档位。
 *
 * ## 只有三档，且没有 Sprint
 * 这是 ZZZ 的设定：移动只有走和跑，冲刺不是独立档位而是能力。
 * 旧实现里 `SprintSpeed` / `SprintMultiplier` 两个配置项从未被任何代码读取，
 * 迁移时一并丢弃，不做保留。
 *
 * ## 与旧 `EMovementGait` 的关系
 * 语义完全相同，改名是为了让新旧移动层能在同一次编译里共存 ——
 * 旧枚举在 `StateMachine/CharacterStateType.h`，那个文件随 StateMachine
 * 一起退役。动画层改用本枚举后旧的就可以删掉。
 */
UENUM(BlueprintType)
enum class EGGYGOGait : uint8
{
	/** 静止，或被禁止移动。速度为 0。 */
	None,

	/** 行走。移动的默认起步档位。 */
	Walk,

	/**
	 * 跑步。
	 *
	 * 进入方式有两条：持续行走满 `WalkToRunHoldSeconds`，
	 * 或闪避结束后立即接移动（`RequestRunOnNextMove` 契约）。
	 * 不会自行回落到 Walk —— 只有完全停止移动才降档，这是单向滞回，
	 * 目的是避免摇杆幅度抖动导致走跑反复切换。
	 */
	Run
};

/**
 * 急停转身（TurnBack）的相位。
 *
 * 跑动中输入接近反向时触发：角色先播一段刹车转身，再朝新方向跑出去。
 * 分相位而不是用一个 bool，是因为这段动作的**可打断性随时间变化**：
 * 前半段（刹车）必须播完，后半段（起跑）可以被新输入接管。
 *
 * ## 当前状态
 * 相位机在阶段 5（移动层重建）中随旧 Pipeline 一并退役，
 * 阶段 6 曲线驱动接入时在 CMC 内重建 —— 它依赖 `RM_Yaw` 动画曲线累加 Actor 朝向，
 * 而曲线采样正是阶段 6 的内容。期间本枚举恒为 `None`，转身表现暂不触发。
 */
UENUM(BlueprintType)
enum class EGGYGOTurnBackPhase : uint8
{
	/** 未在转身。 */
	None,

	/**
	 * 第一段：刹车转身，不可打断。
	 *
	 * 这段的 Actor 朝向由动画曲线驱动，期间会临时关掉 CMC 的自动朝向对齐 ——
	 * 两者同时生效会互相争夺 Yaw，表现为角色抽动。
	 */
	Frozen,

	/**
	 * 第二段：已解冻，可被新输入接管。
	 *
	 * 进入本相位不代表立刻交还控制权，真正的交接由动画里的 `CanYaw` Notify 触发。
	 * 用 Notify 而不是固定时间点，是为了让交接时机跟着动画走 ——
	 * 换一版转身动画时不必回来改配置里的秒数。
	 */
	Released
};

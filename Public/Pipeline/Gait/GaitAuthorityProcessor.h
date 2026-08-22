/**
 * @file GaitAuthorityProcessor.h
 * @brief 逻辑侧步态决策者（Gait_Authority）
 *
 * 负责在单一编译单元内解析步态、推进 Walk_Hold_Timer，并将结果写入
 * RuntimeData.Gait.ResolvedGait 与 RuntimeData.ZZZAnim.Gait。
 *
 * 该类不实现 IIntentProcessor 或 IParameterProcessor：两个接口无法同时
 * 提供 FInputData 与 DeltaTime，由 FIntentPipeline 以具名成员直接持有。
 */
#pragma once

#include "CoreMinimal.h"
#include "StateMachine/CharacterStateType.h"

class ACharacter;
class ABaseCharacter;
class FInputData;
struct FRuntimeData;

/**
 * 步态决策者（Gait_Authority）。
 *
 * RuntimeData.Gait.ResolvedGait 与 RuntimeData.ZZZAnim.Gait 的唯一写入方。
 * 判定输入只有 Move_Input_Present、State.CurrentState、Arbiter.bBlockMove 与
 * Gait.bDodgeRunPending，不读取任何速度类字段。
 */
class FGaitAuthorityProcessor
{
public:
	/** 缓存 Owner，用于每帧读取 FMovementConfig::WalkToRunHoldSeconds。 */
	void Init(ACharacter* InOwner);

	/** 每帧解析步态：帧间隔解析 → 基线步态 → 计时归零与推进 → 升级 → 单次写入。 */
	void Process(const FInputData& InputData, FRuntimeData& RuntimeData, float DeltaTime);

private:
	/** 解析本次生效阈值：非法回退 5.0，超上界钳到 60.0，合法区间取原值。 */
	float ResolveEffectiveThreshold();

	/** 帧间隔钳制到 [0.0, 0.1]；负值或非有限返回 false 并输出节流诊断。 */
	bool ResolveFrameDelta(float InDeltaTime, float& OutClampedDelta);

	/** 基线步态优先级链；必要时消费 Dodge_Run_Contract（唯一置假处）。 */
	EMovementGait ResolveFrameGait(
		FRuntimeData& RuntimeData,
		bool bMoveInputPresent,
		bool bLeftMovingState);

	/**
	 * 先执行至多一次归零，再执行至多一次推进。
	 *
	 * Requirement 9.3 优先于全部归零条件：帧间隔无效时跳过整个计时阶段，
	 * 既不归零也不推进，Walk_Hold_Timer 保持不变；基线步态解析与升级
	 * 判定照常执行，升级使用未变的计时值。
	 */
	void UpdateWalkHoldTimer(
		EMovementGait FrameGait,
		bool bMoveInputPresent,
		bool bBlockMove,
		bool bMovingState,
		bool bMoveInputRising,
		bool bBlockReleased,
		float ClampedDelta,
		bool bDeltaValid);

	/** Walk 保持达到生效阈值时的升级判定。 */
	bool ShouldUpgradeToRun(
		EMovementGait FrameGait,
		bool bMoveInputPresent,
		bool bBlockMove);

	/** 所属角色（读取 UCharConfigData::MovementConfig 用）。 */
	TWeakObjectPtr<ABaseCharacter> Owner;

	/**
	 * Walk_Hold_Timer：跨帧计时，恒落在 [0, 3600]，不进入 FRuntimeData，
	 * 也不参与 ResetFrameIntents()。
	 */
	float WalkHoldTimer = 0.0f;

	/** 上一帧最终解析出的步态，用于与当前判定结果比较并识别步态变化。 */
	EMovementGait PreviousResolvedGait = EMovementGait::None;
	/** 上一帧是否存在有效移动输入，用于维持跨帧判定上下文。 */
	bool bPreviousMoveInputPresent = false;
	/** 上一帧是否处于阻止移动的状态，用于参与当前帧步态判定。 */
	bool bPreviousBlockMove = false;
	/** 上一帧角色状态，用于检测状态变化对步态判定的影响。 */
	ECharacterStateType PreviousCurrentState = ECharacterStateType::Idle;

	/** 诊断节流标记：条件恢复正常时复位，避免每帧刷屏。 */
	/** 是否已经报告过无效时间增量，避免同一异常在连续帧重复输出。 */
	bool bInvalidDeltaReported = false;
	/** 是否已经报告过阈值回退，避免阈值配置异常时每帧重复输出。 */
	bool bThresholdFallbackReported = false;
	/** 是否已经报告过阈值钳制，避免阈值超出允许范围时每帧重复输出。 */
	bool bThresholdClampReported = false;
	/** 是否已经报告过缺少 Owner，避免对象关联异常时每帧重复输出。 */
	bool bMissingOwnerReported = false;

	/**
	 * 阈值常量（逻辑侧自持，不复用动画层规则层常量）。
	 * Requirement 10.4 要求这些常量从动画层规则层移除，逻辑侧不应对
	 * 动画层头文件产生新的依赖方向。
	 */
	/** 默认的走路转跑步保持时长，作为未提供有效配置时的回退值。 */
	static constexpr float DefaultWalkToRunHoldSeconds = 5.0f;
	/** 走路转跑步保持时长的上限，用于限制外部配置带来的等待时间。 */
	static constexpr float MaxWalkToRunHoldSeconds = 60.0f;
	/** 走路保持时长的上限，用于限制累计保持时间的最大范围。 */
	static constexpr float MaxWalkHoldSeconds = 3600.0f;
	/** 时间增量允许使用的最大钳制值，用于隔离异常帧间隔对判定的影响。 */
	static constexpr float MaxClampedDelta = 0.1f;
};

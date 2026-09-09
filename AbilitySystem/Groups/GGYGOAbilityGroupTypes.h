/**
 * @file GGYGOAbilityGroupTypes.h
 * @brief Ability 并发控制的三个正交维度
 *
 * 替代 Lyra 的 `ELyraAbilityActivationGroup`。Lyra 那套只有三个值
 * （Independent / Exclusive_Replaceable / Exclusive_Blocking），判定实现是一个
 * 三元素计数数组，逻辑只有"有 Blocking 就拒绝所有 Exclusive"，因此有两个硬伤：
 *   1. 只有一个"独占"概念，没有"组"，表达不了"技能之间互斥但技能和普攻可共存"
 *   2. 没有优先级数值，Replaceable 之间后到者无条件取消先到者 —— 大招会被普攻打断
 *
 * 本项目把并发控制拆成三个正交维度：
 *
 * | 维度 | 配在哪 | 作用 |
 * |---|---|---|
 * | 组身份 `GroupTag`   | GA 资产      | 这个 GA 属于哪个组 |
 * | 组规则 `GroupRule`  | 独立 DataAsset | 该组内部的并发规则，改规则只改一处 |
 * | 自身策略 + 优先级    | GA 资产      | 个体行为与冲突时的强弱 |
 *
 * ## 判定入口
 * 三个维度最终在 `UGGYGOAbilitySystemComponent::IsActivationBlockedByGroup` 汇合：
 * 先算跨组的 `SelfPolicy` 排斥，再查 `UGGYGOAbilityGroupConfig` 得到本组规则。
 * 未注入配置表时所有组走 `FGGYGOAbilityGroupRule` 的字段默认值。
 */
#pragma once

#include "GameplayTagContainer.h"

#include "GGYGOAbilityGroupTypes.generated.h"

/**
 * 组内部的并发规则。配在组上（DataAsset），不配在 GA 上。
 */
UENUM(BlueprintType)
enum class EGGYGOAbilityGroupRule : uint8
{
	/** 组内任意多个可同时激活。用于 Buff、被动。 */
	Coexist,

	/**
	 * 组内同时只能一个，按优先级决定去留。用于技能组、大招组。
	 * 优先级相同时的行为由 `bNewcomerWinsOnTie` 决定。
	 */
	SingleInstance,

	/**
	 * 组内同时只能一个，且**从不取消已有实例** —— 新请求直接被拒，等旧的自然结束。
	 * 这是普攻连段的正确机制：第二段等第一段播完再出，而不是打断它重头播。
	 *
	 * 与 `SingleInstance` 的区别就在"取不取消"：
	 * `SingleInstance` 会按优先级顶掉旧的，本规则不比较优先级、只看有没有人在跑。
	 *
	 * "排队"本身不在 ASC 里实现。ASC 只负责拒绝并给出
	 * `EGGYGOAbilityGroupBlockReason::GroupOccupiedQueued`；
	 * 真正的重试由意图层的输入缓冲在有效窗内完成（阶段 7），
	 * 也可以订阅 ASC 的 `OnAbilityGroupFreed` 在组空出的瞬间立即重试。
	 * 这样避免了 ASC 与意图层各持一个队列。
	 */
	SingleInstanceQueued
};

/**
 * GA 自身的排斥策略。配在 GA 资产上。
 */
UENUM(BlueprintType)
enum class EGGYGOAbilitySelfPolicy : uint8
{
	/** 不主动排斥任何人，只受所在组的规则约束。 */
	Coexist,

	/**
	 * 激活期间排斥**所有组**的低优先级 GA，不只是同组。
	 * 用于死亡、被击倒、大招这类"播放期间世界静止"的能力。
	 */
	Exclusive
};

/**
 * 组仲裁拒绝激活的原因。
 *
 * 区分原因的用途：`GroupOccupiedQueued` 表示"这次不行但请求值得保留"，
 * 调用方（意图层的输入缓冲）可以在有效窗内继续重试；
 * 其余原因表示"条件不满足"，重试也没意义。
 */
UENUM(BlueprintType)
enum class EGGYGOAbilityGroupBlockReason : uint8
{
	/** 未被组规则阻断。 */
	NotBlocked,

	/** 存在跨组的 `Exclusive` 高优先级能力（死亡、被击倒、大招演出中）。 */
	ExclusiveActive,

	/** 同组内有优先级更高的能力正在运行。 */
	LowerPriority,

	/**
	 * 同组规则是 `SingleInstanceQueued` 且组内已有实例。
	 * 与 `LowerPriority` 的区别：这里**不比较优先级**，也**不取消**已有实例，
	 * 是严格的先来后到。连段就靠它实现。
	 */
	GroupOccupiedQueued
};

/** 一个组的并发规则配置。由 `UGGYGOAbilityGroupConfig` 按 GroupTag 索引。 */
USTRUCT(BlueprintType)
struct FGGYGOAbilityGroupRule
{
	GENERATED_BODY()

	/** 该组的并发规则。 */
	UPROPERTY(EditAnywhere, Category = "Group")
	EGGYGOAbilityGroupRule Rule = EGGYGOAbilityGroupRule::SingleInstance;

	/**
	 * 优先级相同时谁胜出。
	 *
	 * - `true`（默认，对应决策 D4）：后来者打断先激活者。同级技能可互断。
	 * - `false`：先到先得，后来者被拒绝。
	 *
	 * 做成组参数而不是全局常量，是因为两种语义在同一个项目里都需要：
	 * 技能组要能互断，而某些组（例如受击反应）更适合先到先得。
	 */
	UPROPERTY(EditAnywhere, Category = "Group", meta = (EditCondition = "Rule == EGGYGOAbilityGroupRule::SingleInstance"))
	bool bNewcomerWinsOnTie = true;
};

/** 组仲裁相关的默认值。 */
namespace GGYGOAbilityGroupDefaults
{
	/**
	 * 优先级建议分段。分段之间留空隙，便于以后在中间插新档位而不用整体重排。
	 *
	 *   死亡 / 强制状态   1000
	 *   被击倒 / 强硬直    800
	 *   大招              600
	 *   技能              400
	 *   闪避              300
	 *   重攻击            200
	 *   轻攻击            100
	 *   被动 / Buff          0
	 */
	constexpr int32 Priority_Death = 1000;
	constexpr int32 Priority_KnockDown = 800;
	constexpr int32 Priority_Ultimate = 600;
	constexpr int32 Priority_Skill = 400;
	constexpr int32 Priority_Dodge = 300;
	constexpr int32 Priority_HeavyAttack = 200;
	constexpr int32 Priority_LightAttack = 100;
	constexpr int32 Priority_Passive = 0;
}

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
 * ## 当前实现范围
 * 阶段 1 只落地类型定义和 GA 上的字段，ASC 里是**最小仲裁**：
 * 全局 Exclusive 排斥 + 同组默认按 `SingleInstance` 处理。
 * 阶段 3 才接入 `UGGYGOAbilityGroupConfig` DataAsset，让每个组能配自己的规则。
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
	 * 组内同时只能一个，但新请求**进输入缓冲队列**而不是取消旧的。
	 * 这是普攻连段的正确机制：第二段等第一段结束再出，而不是打断它重头播。
	 * 队列由意图层的 `InputBufferQueue` 持有（阶段 7 实现）。
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

/** 一个组的并发规则配置。由 `UGGYGOAbilityGroupConfig`（阶段 3）按 GroupTag 索引。 */
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

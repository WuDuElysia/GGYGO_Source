/**
 * @file GGYGOGameplayTags.h
 * @brief 原生 GameplayTag 声明（阶段 1 范围）
 *
 * 与旧的 `GGYGOTags.h` 的区别：
 *   - 旧文件用 `namespace + static const FName` 保存字符串，运行时靠 `RequestGameplayTag` 解析，
 *     再用 `FGGYGOTagCache` 手工缓存；这条链路有运行时字符串查找，且 Tag 是否存在只能在运行时发现。
 *   - 本文件用 `UE_DECLARE_GAMEPLAY_TAG_EXTERN` / `UE_DEFINE_GAMEPLAY_TAG` 原生声明，
 *     Tag 在模块加载时注册，编译期即可引用符号，无字符串查找，也不需要额外缓存结构。
 *
 * 当前只声明阶段 1（GAS 核心层）实际用到的 Tag。`State.*`、`Restriction.*`、`Ability.Melee` 等
 * 仍留在 `Config/DefaultGameplayTags.ini` 与旧 `GGYGOTags.h` 中，等阶段 2 统一迁移。
 */
#pragma once

#include "NativeGameplayTags.h"

class FString;

namespace GGYGOGameplayTags
{
	/**
	 * 按字符串查找已注册的 Tag。
	 * @param TagString           完整 Tag 名，例如 "Ability.ActivateFail.Cost"。
	 * @param bMatchPartialString 为 true 时在精确匹配失败后退化为子串搜索，仅用于调试与作弊指令。
	 * @return 命中的 Tag；未命中时返回无效 Tag，调用方须自行判断 `IsValid()`。
	 */
	GGYGO_API FGameplayTag FindTagByString(const FString& TagString, bool bMatchPartialString = false);

	// ============================================================
	// 一、Ability 激活失败原因
	// 由 UGGYGOGameplayAbility::CanActivateAbility 及 ASC 的标签关系检查写入
	// OptionalRelevantTags，再交给失败反馈（文本 / Montage）消费。
	// ============================================================

	/** 角色已死亡，拒绝激活。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_ActivateFail_IsDead);
	/** 能力处于冷却中。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_ActivateFail_Cooldown);
	/** 资源不足，未通过 Cost 检查。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_ActivateFail_Cost);
	/** 被 ASC 上的阻断 Tag 拦截。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_ActivateFail_TagsBlocked);
	/** 缺少必需 Tag。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_ActivateFail_TagsMissing);
	/** 网络角色不满足激活条件。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_ActivateFail_Networking);
	/** 被激活组 / 组优先级仲裁拒绝。阶段 3 的三维仲裁失败也复用此 Tag。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_ActivateFail_ActivationGroup);

	// ============================================================
	// 二、Ability 行为标记
	// ============================================================

	/** 标记该能力在拥有者死亡时不被清除，用于死亡演出本身等能力。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Behavior_SurvivesDeath);

	// ============================================================
	// 三、伤害与韧性
	// ============================================================

	/** 普通伤害来源标记。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Gameplay_Damage);
	/** 免疫普通伤害。闪避无敌帧施加此 Tag。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Gameplay_Damage_Immunity);
	/** 自毁 / 处死类伤害，绕过免疫与开发期保命规则。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Gameplay_Damage_SelfDestruct);
	/** 削韧来源标记。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Gameplay_PoiseDamage);

	// ============================================================
	// 四、状态
	// `State.Dead` 由本文件原生注册，已从 DefaultGameplayTags.ini 移除，避免两处定义。
	// 其余 `State.*` 仍在 ini 中，阶段 2 一并迁移。
	// ============================================================

	/** 角色已死亡。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Dead);
	/** 韧性被击破，处于破韧硬直中。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_PoiseBreak);

	// ============================================================
	// 五、SetByCaller 幅度键
	// GE 用 `SetSetByCallerMagnitude` 传入具体数值时的键名。
	// ============================================================

	/** 伤害数值的 SetByCaller 键。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(SetByCaller_Damage);
	/** 治疗数值的 SetByCaller 键。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(SetByCaller_Heal);
	/** 削韧数值的 SetByCaller 键。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(SetByCaller_PoiseDamage);

	// ============================================================
	// 六、跨系统消息 Verb
	// 配合 GameplayMessageSubsystem 广播给 UI / 音频 / 表现层的只读观察者。
	// ============================================================

	/** 伤害消息。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Message_Damage);
	/** 破韧消息。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Message_PoiseBreak);

	// ============================================================
	// 七、开发期作弊
	// 仅在非 Shipping 构建中生效。
	// ============================================================

	/** 免疫一切普通伤害。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cheat_GodMode);
	/** 生命值不会低于 1。 */
	GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cheat_UnlimitedHealth);
}

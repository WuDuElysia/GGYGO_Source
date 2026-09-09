/**
 * @file GGYGOTags.h
 * @brief 集中式 GameplayTag 定义（Phase 7）
 *
 * 所有 Tag 在此文件统一定义，避免散落在各处的字符串硬编码。
 * 使用 namespace + static const 模式，Init 时一次性解析为 FGameplayTag。
 *
 * Tag 命名规范（参考 NTE）：
 *   State.XXX        — 状态身份 Tag（状态进入时添加，退出时移除）
 *   Restriction.XXX  — 功能限制 Tag（由 GE 施加，Arbiter 读取后设 bBlock*）
 *   Ability.XXX      — 技能相关 Tag
 *   Cooldown.XXX      — 冷却 Tag（GE_Cooldown* 类自带）
 *   Event.XXX         — 事件通知 Tag（UI/Camera/Audio 只读消费）
 */
#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"

namespace GGYGOTags
{
	// ============================================================
	// 一、状态身份 Tags (State.*)
	// 由状态机 ActivateState/DeactivateState 自动管理
	// 用途：外部系统查询"角色当前是否在 XX 状态"
	// ============================================================

	namespace State
	{
		static const FName Idle       = "State.Idle";
		static const FName Moving     = "State.Moving";
		static const FName InAir      = "State.InAir";
		static const FName Attacking  = "State.Attacking";
		static const FName Dodging    = "State.Dodging";
		static const FName HitStun    = "State.HitStun";
		static const FName Stunned    = "State.Stunned";
		static const FName Dead       = "State.Dead";
		static const FName Interacting= "State.Interacting";
	}

	// ============================================================
	// 二、功能限制 Tags (Restriction.*)
	// 由 GE 施加，GASArbiter / 输入系统读取
	// GE 的 OwnedTags 包含这些 Tag → 对应功能被禁用
	// ============================================================

	namespace Restriction
	{
		static const FName CantMove      = "Restriction.CantMove";       // 不能移动
		static const FName CantAttack     = "Restriction.CantAttack";     // 不能攻击
		static const FName CantDodge      = "Restriction.CantDodge";      // 不能闪避
		static const FName CantJump       = "Restriction.CantJump";       // 不能跳跃
		static const FName CantInput      = "Restriction.CantInput";      // 禁止所有输入处理
		static const FName CantLookInput  = "Restriction.CantLookInput";  // 禁止视角控制
		static const FName CantInteract   = "Restriction.CantInteract";   // 禁止交互
		static const FName ImmuneDamage   = "Restriction.ImmuneDamage";   // 免疫伤害
	}

	// ============================================================
	// 三、技能 Tags (Ability.*)
	// 技能激活/冷却/打断时使用
	// ============================================================

	namespace Ability
	{
		static const FName Melee     = "Ability.Melee";       // 近战攻击
		static const FName Dodge     = "Ability.Dodge";       // 闪避
		static const FName Skill     = "Ability.Skill";       // 技能
		static const FName UltraSkill= "Ability.UltraSkill";   // 大招
		static const FName IsChanneling = "Ability.IsChanneling"; // 正在引导中
	}

	// ============================================================
	// 四、冷却 Tags (Cooldown.*)
	// GE_Cooldown* 类的 OwnedTags，用于检测技能是否在 CD 中
	// ============================================================

	namespace Cooldown
	{
		static const FName Evade  = "Cooldown.Evade";   // 闪避 CD
		static const FName Attack = "Cooldown.Attack";   // 攻击 CD
		static const FName Skill  = "Cooldown.Skill";    // 技能 CD
	}

	// ============================================================
	// 五、事件通知 Tags (Event.*)
	// 只读 Tag，UI/Camera/Audio 系统响应式消费
	// ============================================================

	namespace Event
	{
		static const FName PerfectEvade = "Event.PerfectEvade";   // 完美闪避
		static const FName TakeDamage    = "Event.TakeDamage";     // 受到伤害
		static const FName Death          = "Event.Death";          // 死亡事件
		static const FName Respawn        = "Event.Respawn";        // 复活事件
	}
}

/**
 * Tag 缓存容器
 * 在 Init 时将所有 FName 解析为 FGameplayTag，避免运行时字符串查找。
 * 所有需要查 Tag 的地方引用此单例即可。
 */
struct FGGYGOTagCache
{
	// ===== State Tags =====
	FGameplayTag State_Idle;
	FGameplayTag State_Moving;
	FGameplayTag State_InAir;
	FGameplayTag State_Attacking;
	FGameplayTag State_Dodging;
	FGameplayTag State_HitStun;
	FGameplayTag State_Stunned;
	FGameplayTag State_Dead;
	FGameplayTag State_Interacting;

	// ===== Restriction Tags =====
	FGameplayTag Restriction_CantMove;
	FGameplayTag Restriction_CantAttack;
	FGameplayTag Restriction_CantDodge;
	FGameplayTag Restriction_CantJump;
	FGameplayTag Restriction_CantInput;
	FGameplayTag Restriction_CantLookInput;
	FGameplayTag Restriction_CantInteract;
	FGameplayTag Restriction_ImmuneDamage;

	// ===== Ability Tags =====
	FGameplayTag Ability_Melee;
	FGameplayTag Ability_Dodge;
	FGameplayTag Ability_Skill;
	FGameplayTag Ability_UltraSkill;
	FGameplayTag Ability_IsChanneling;

	// ===== Cooldown Tags =====
	FGameplayTag Cooldown_Evade;
	FGameplayTag Cooldown_Attack;
	FGameplayTag Cooldown_Skill;

	// ===== Event Tags =====
	FGameplayTag Event_PerfectEvade;
	FGameplayTag Event_TakeDamage;
	FGameplayTag Event_Death;
	FGameplayTag Event_Respawn;

	/**
	 * 初始化：将所有 FName 解析为 FGameplayTag
	 * 应在游戏启动时调用一次（如 BeginPlay 或子系统初始化时）
	 */
	void Init()
	{
		using namespace GGYGOTags;

		// State
		State_Idle       = FGameplayTag::RequestGameplayTag(State::Idle);
		State_Moving     = FGameplayTag::RequestGameplayTag(State::Moving);
		State_InAir      = FGameplayTag::RequestGameplayTag(State::InAir);
		State_Attacking  = FGameplayTag::RequestGameplayTag(State::Attacking);
		State_Dodging    = FGameplayTag::RequestGameplayTag(State::Dodging);
		State_HitStun    = FGameplayTag::RequestGameplayTag(State::HitStun);
		State_Stunned    = FGameplayTag::RequestGameplayTag(State::Stunned);
		State_Dead       = FGameplayTag::RequestGameplayTag(State::Dead);
		State_Interacting= FGameplayTag::RequestGameplayTag(State::Interacting);

		// Restriction
		Restriction_CantMove     = FGameplayTag::RequestGameplayTag(Restriction::CantMove);
		Restriction_CantAttack    = FGameplayTag::RequestGameplayTag(Restriction::CantAttack);
		Restriction_CantDodge     = FGameplayTag::RequestGameplayTag(Restriction::CantDodge);
		Restriction_CantJump      = FGameplayTag::RequestGameplayTag(Restriction::CantJump);
		Restriction_CantInput     = FGameplayTag::RequestGameplayTag(Restriction::CantInput);
		Restriction_CantLookInput = FGameplayTag::RequestGameplayTag(Restriction::CantLookInput);
		Restriction_CantInteract  = FGameplayTag::RequestGameplayTag(Restriction::CantInteract);
		Restriction_ImmuneDamage  = FGameplayTag::RequestGameplayTag(Restriction::ImmuneDamage);

		// Ability
		Ability_Melee       = FGameplayTag::RequestGameplayTag(Ability::Melee);
		Ability_Dodge       = FGameplayTag::RequestGameplayTag(Ability::Dodge);
		Ability_Skill       = FGameplayTag::RequestGameplayTag(Ability::Skill);
		Ability_UltraSkill  = FGameplayTag::RequestGameplayTag(Ability::UltraSkill);
		Ability_IsChanneling = FGameplayTag::RequestGameplayTag(Ability::IsChanneling);

		// Cooldown
		Cooldown_Evade  = FGameplayTag::RequestGameplayTag(Cooldown::Evade);
		Cooldown_Attack = FGameplayTag::RequestGameplayTag(Cooldown::Attack);
		Cooldown_Skill  = FGameplayTag::RequestGameplayTag(Cooldown::Skill);

		// Event
		Event_PerfectEvade = FGameplayTag::RequestGameplayTag(Event::PerfectEvade);
		Event_TakeDamage    = FGameplayTag::RequestGameplayTag(Event::TakeDamage);
		Event_Death          = FGameplayTag::RequestGameplayTag(Event::Death);
		Event_Respawn        = FGameplayTag::RequestGameplayTag(Event::Respawn);
	}
};

/**
 * @file GGYGOGameplayEffects.h
 * @brief GameplayEffect 类引用注册表
 *
 * 集中存放所有 GE 的 `TSoftClassPtr`。GE 蓝图类本身在 `Content/` 下创建，
 * 这里提供统一的引用入口，让 C++ 侧不必硬编码资产路径字符串。
 *
 * `InitGEGlobals()` 必须在任何 GE 被应用之前调用一次。
 * 目前没有调用方 —— 原先由角色 BeginPlay 触发，那条链路已不存在，
 * 归属应当是游戏或世界级的初始化点。在补上之前这里的引用全部为空。
 */
#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"

/**
 * 全局 GE 类引用（TSoftClassPtr，支持异步加载）
 * 在游戏初始化时同步加载到内存
 */
namespace GGYGOGEs
{
	// ============================================================
	// 一、限制型 GE（施加 Restriction.* Tag）
	// DurationPolicy = Infinite（状态退出时手动移除）
	// StackLimitCount = 1（同类型不叠加）
	// ============================================================

	namespace Restriction
	{
		/** 封锁全部操作（死亡/过场动画）*/
		extern GGYGO_API TSoftClassPtr<UGameplayEffect> BlockAll;

		/** 封锁战斗操作：攻击+闪避+跳跃（受击硬直）*/
		extern GGYGO_API TSoftClassPtr<UGameplayEffect> BlockCombat;

		/** 仅禁止移动（交互中）*/
		extern GGYGO_API TSoftClassPtr<UGameplayEffect> BlockMoveOnly;

		/** 无敌帧（闪避期间免疫伤害）*/
		extern GGYGO_API TSoftClassPtr<UGameplayEffect> Invincible;
	}

	// ============================================================
	// 二、冷却型 GE（Duration = 具体秒数，自动过期）
	// OwnedTags 包含 Cooldown.* Tag
	// ============================================================

	namespace Cooldown
	{
		/** 闪避冷却（Duration ≈ 0.9s）*/
		extern GGYGO_API TSoftClassPtr<UGameplayEffect> CooldownEvade;

		/** 技能冷却（Duration 由 CurveTable 驱动）*/
		extern GGYGO_API TSoftClassPtr<UGameplayEffect> CooldownSkill;
	}

	// ============================================================
	// 三、属性修改型 GE（修改 AttributeSet 数值）
	// ============================================================

	namespace Attribute
	{
		/** 受伤减速（MoveSpeed 乘以 0.5）*/
		extern GGYGO_API TSoftClassPtr<UGameplayEffect> InjuredSlowdown;
	}
}

/**
 * 初始化所有 GE 类引用（将 SoftPtr 解析为硬引用）
 * 必须在 BeginPlay 中调用，在任何 GE 应用之前
 */
extern GGYGO_API void InitGEGlobals();

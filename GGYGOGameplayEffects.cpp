/**
 * @file GGYGOGameplayEffects.cpp
 * @brief GameplayEffect 类引用注册表实现
 */
#include "GGYGOGameplayEffects.h"

namespace GGYGOGEs
{
	namespace Restriction
	{
		// ★ 蓝图类路径 — 用户在编辑器创建对应 GE 资产后更新此路径
		TSoftClassPtr<UGameplayEffect> BlockAll(
			FSoftObjectPath(TEXT("/Game/BP/GAS/BP_GE/GE_BlockAll.GE_BlockAll_C")));

		TSoftClassPtr<UGameplayEffect> BlockCombat(
			FSoftObjectPath(TEXT("/Game/BP/GAS/BP_GE/GE_BlockCombat.GE_BlockCombat_C")));

		TSoftClassPtr<UGameplayEffect> BlockMoveOnly(
			FSoftObjectPath(TEXT("/Game/BP/GAS/BP_GE/GE_BlockMoveOnly.GE_BlockMoveOnly_C")));

		TSoftClassPtr<UGameplayEffect> Invincible(
			FSoftObjectPath(TEXT("/Game/BP/GAS/BP_GE/GE_Invincible.GE_Invincible_C")));
	}

	namespace Cooldown
	{
		TSoftClassPtr<UGameplayEffect> CooldownEvade(
			FSoftObjectPath(TEXT("/Game/BP/GAS/BP_GE/GE_CooldownEvade.GE_CooldownEvade_C")));

		TSoftClassPtr<UGameplayEffect> CooldownSkill(
			FSoftObjectPath(TEXT("/Game/BP/GAS/BP_GE/GE_CooldownSkill.GE_CooldownSkill_C")));
	}

	namespace Attribute
	{
		TSoftClassPtr<UGameplayEffect> InjuredSlowdown(
			FSoftObjectPath(TEXT("/Game/BP/GAS/BP_GE/GE_InjuredSlowdown.GE_InjuredSlowdown_C")));
	}
}

void InitGEGlobals()
{
	using namespace GGYGOGEs;

	// 同步加载所有 GE 类引用（游戏启动时一次性完成）
	Restriction::BlockAll.LoadSynchronous();
	Restriction::BlockCombat.LoadSynchronous();
	Restriction::BlockMoveOnly.LoadSynchronous();
	Restriction::Invincible.LoadSynchronous();

	Cooldown::CooldownEvade.LoadSynchronous();
	Cooldown::CooldownSkill.LoadSynchronous();

	Attribute::InjuredSlowdown.LoadSynchronous();
}

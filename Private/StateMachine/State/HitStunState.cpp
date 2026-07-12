/**
 * @file HitStunState.cpp
 * @brief 受击硬直状态实现
 *
 * 阶段7 GAS 接入：
 *   Enter → 施加 GE_BlockCombat（禁止攻击/闪避/跳跃）+ GE_InjuredSlowdown（减速）
 *   Exit → 自动移除 Infinite GE（由 StateManager 通过 RemoveGEsWithTag 处理）
 */
#include "StateMachine/State/HitStunState.h"
#include "GGYGOGameplayEffects.h"
#include "GGYGOTags.h"

TArray<TSubclassOf<UGameplayEffect>> FHitStunState::GetEnterGameplayEffects() const
{
	return {
		GGYGOGEs::Restriction::BlockCombat.Get(),     // 禁止战斗操作
		GGYGOGEs::Attribute::InjuredSlowdown.Get(),    // 受伤减速
	};
}

int32 FHitStunState::GetLimitFlags() const
{
	// CantMove + CantAttack + CantDodge + CantJump
	return (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3);
}

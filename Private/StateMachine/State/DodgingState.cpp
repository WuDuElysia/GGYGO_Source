/**
 * @file DodgingState.cpp
 * @brief 闪避状态实现
 *
 * 阶段7 GAS 接入：
 *   Enter → 施加 GE_Invincible（无敌帧）+ GE_CooldownEvade（闪避CD）
 *   Exit → 移除 Invincible（CD GE 自动过期）
 */
#include "StateMachine/State/DodgingState.h"
#include "GGYGOGameplayEffects.h"
#include "GGYGOTags.h"

TArray<TSubclassOf<UGameplayEffect>> FDodgingState::GetEnterGameplayEffects() const
{
	return {
		GGYGOGEs::Restriction::Invincible.Get(),       // 无敌帧
		GGYGOGEs::Cooldown::CooldownEvade.Get(),         // 闪避CD
	};
}

FGameplayTagContainer FDodgingState::GetRemoveGEsWithTag() const
{
	FGameplayTagContainer Tags;
	Tags.AddTag(FGameplayTag::RequestGameplayTag(GGYGOTags::Restriction::ImmuneDamage));
	return Tags;
}

int32 FDodgingState::GetLimitFlags() const
{
	// CantAttack + CantDodge + CantJump（闪避中不能攻击/再闪避/跳跃）
	return (1 << 1) | (1 << 2) | (1 << 3);
}

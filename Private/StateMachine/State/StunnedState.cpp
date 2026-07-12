/**
 * @file StunnedState.cpp
 * @brief 眩晕状态实现
 *
 * 阶段7 GAS 接入：
 *   Enter → 施加 GE_BlockAll（禁止一切操作，除了输入处理）
 */
#include "StateMachine/State/StunnedState.h"
#include "GGYGOGameplayEffects.h"
#include "GGYGOTags.h"

TArray<TSubclassOf<UGameplayEffect>> FStunnedState::GetEnterGameplayEffects() const
{
	return {
		GGYGOGEs::Restriction::BlockAll.Get(),  // 封锁全部操作
	};
}

int32 FStunnedState::GetLimitFlags() const
{
	// CantMove + CantAttack + CantDodge + CantJump + CantInput
	return (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4);
}

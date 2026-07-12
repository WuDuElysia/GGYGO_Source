/**
 * @file DeadState.cpp
 * @brief 死亡状态实现（终态）
 *
 * 阶段7 GAS 接入：
 *   Enter → 施加 GE_BlockAll（禁止一切操作，终态不可退出）
 *   Dead 是终态：一旦进入，只有 ForceSetPrimaryState 或外部重置才能离开
 */
#include "StateMachine/State/DeadState.h"
#include "GGYGOGameplayEffects.h"
#include "GGYGOTags.h"

TArray<TSubclassOf<UGameplayEffect>> FDeadState::GetEnterGameplayEffects() const
{
	return {
		GGYGOGEs::Restriction::BlockAll.Get(),  // 封锁全部操作
	};
}

int32 FDeadState::GetLimitFlags() const
{
	// 全部封锁
	return (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5);
}

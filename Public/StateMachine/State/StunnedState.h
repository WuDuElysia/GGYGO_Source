/**
 * @file StunnedState.h
 * @brief 眩晕状态
 *
 * 角色被眩晕（如被重击或特殊技能命中）。
 * TODO: 阶段六仲裁管线 + 阶段八 GAS 接入后，由眩晕 GE 触发进入。
 */
#pragma once

#include "StateMachine/CharacterState.h"

class FStunnedState : public FCharacterState
{
public:
	FStunnedState()
		: FCharacterState(ECharacterStateType::Stunned, EStateGroup::Overlay)
	{
	}

	// Phase 7 GAS 接入：眩晕时封锁全部操作
	virtual TArray<TSubclassOf<UGameplayEffect>> GetEnterGameplayEffects() const override;
	virtual int32 GetLimitFlags() const override;
};

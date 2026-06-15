/**
 * @file DeadState.h
 * @brief 死亡状态（终态）
 *
 * 角色死亡后的状态，不允许任何转换。
 * TODO: 阶段六仲裁管线 + 阶段八 GAS 接入后，由血量仲裁触发进入。
 */
#pragma once

#include "StateMachine/CharacterState.h"

class FDeadState : public FCharacterState
{
public:
	FDeadState()
		: FCharacterState(ECharacterStateType::Dead)
	{
	}
};

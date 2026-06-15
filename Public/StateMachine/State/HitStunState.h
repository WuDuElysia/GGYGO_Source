/**
 * @file HitStunState.h
 * @brief 受击硬直状态
 *
 * 角色受到攻击后进入短硬直。
 * TODO: 阶段六仲裁管线 + 阶段八 GAS 接入后，由受击 GE 触发进入。
 */
#pragma once

#include "StateMachine/CharacterState.h"

class FHitStunState : public FCharacterState
{
public:
	FHitStunState()
		: FCharacterState(ECharacterStateType::HitStun)
	{
	}
};

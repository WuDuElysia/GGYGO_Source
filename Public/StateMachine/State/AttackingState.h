/**
 * @file AttackingState.h
 * @brief 攻击状态
 *
 * 角色正在攻击中（由 GAS GA 驱动）。
 * TODO: 阶段八 GAS 接入后，攻击开始/结束由 GA 激活/终止驱动。
 */
#pragma once

#include "StateMachine/CharacterState.h"

class FAttackingState : public FCharacterState
{
public:
	FAttackingState()
		: FCharacterState(ECharacterStateType::Attacking, EStateGroup::Action)
	{
	}
};

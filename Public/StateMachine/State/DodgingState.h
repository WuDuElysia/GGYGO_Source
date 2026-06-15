/**
 * @file DodgingState.h
 * @brief 闪避状态
 *
 * 角色正在闪避中（由 GAS GA 驱动）。
 * TODO: 阶段八 GAS 接入后，闪避开始/结束由 GA 激活/终止驱动。
 */
#pragma once

#include "StateMachine/CharacterState.h"

class FDodgingState : public FCharacterState
{
public:
	FDodgingState()
		: FCharacterState(ECharacterStateType::Dodging)
	{
	}
};

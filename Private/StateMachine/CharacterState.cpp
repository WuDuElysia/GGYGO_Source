/**
 * @file CharacterState.cpp
 * @brief 角色状态基类实现
 */
#include "StateMachine/CharacterState.h"

FCharacterState::FCharacterState(ECharacterStateType InType)
	: StateType(InType)
{
}

FCharacterState::~FCharacterState()
{
}

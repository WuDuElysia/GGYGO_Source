/**
 * @file StateRuntimeModel.h
 * @brief 逻辑状态机运行时模型
 */
#pragma once

#include "StateMachine/CharacterStateType.h"

/** StateManager 维护的当前主状态。 */
struct FStateRuntimeModel
{
	ECharacterStateType CurrentState = ECharacterStateType::Idle;
};

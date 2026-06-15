/**
 * @file InAirState.h
 * @brief 空中状态
 *
 * 角色跳跃后处于空中。
 * 落地检测 → 回到 Idle（无移动输入）或 Locomotion（有移动输入）。
 */
#pragma once

#include "StateMachine/CharacterState.h"

class FInAirState : public FCharacterState
{
public:
	FInAirState()
		: FCharacterState(ECharacterStateType::InAir)
	{
	}

	virtual void Update(float DeltaTime, FRuntimeData& RuntimeData, FCharacterStateMachine& SM) override;
};

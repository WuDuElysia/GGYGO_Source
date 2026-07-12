/**
 * @file IdleState.h
 * @brief 待机状态
 *
 * 角色静止时的默认状态。
 * 检测移动/跳跃/攻击/闪避/冲刺意图 → 切换到对应状态。
 */
#pragma once

#include "StateMachine/CharacterState.h"

class FIdleState : public FCharacterState
{
public:
	FIdleState()
		: FCharacterState(ECharacterStateType::Idle, EStateGroup::Locomotion)
	{
	}

	virtual void Enter(FRuntimeData& RuntimeData) override;
	virtual void Update(float DeltaTime, FRuntimeData& RuntimeData, FGYGOStateManager& SM) override;
	virtual void Exit(FRuntimeData& RuntimeData) override;
};

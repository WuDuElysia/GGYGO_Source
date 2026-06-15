/**
 * @file RunEndState.h
 * @brief 跑步停止状态
 *
 * RunStart 中松手 或 RunLoop 中松手 → RunEnd（播放停止动画，非循环）。
 * 动画播完 → Idle。
 * 动画播放期间又开始移动 → 打断，回到 RunStart。
 */
#pragma once

#include "StateMachine/CharacterState.h"

class FRunEndState : public FCharacterState
{
public:
	FRunEndState()
		: FCharacterState(ECharacterStateType::RunEnd)
	{
	}

	virtual void Enter(FRuntimeData& RuntimeData) override;
	virtual void Update(float DeltaTime, FRuntimeData& RuntimeData, FCharacterStateMachine& SM) override;
	virtual void Exit(FRuntimeData& RuntimeData) override;
};

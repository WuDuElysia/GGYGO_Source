/**
 * @file RunLoopState.h
 * @brief 跑步循环状态
 *
 * RunStart 启动完成 → RunLoop（循环播放跑步动画）。
 * 松手 → RunEnd，跳跃/闪避/攻击 → 对应状态。
 */
#pragma once

#include "StateMachine/CharacterState.h"

class FRunLoopState : public FCharacterState
{
public:
	FRunLoopState()
		: FCharacterState(ECharacterStateType::RunLoop)
	{
	}

	virtual void Update(float DeltaTime, FRuntimeData& RuntimeData, FCharacterStateMachine& SM) override;
};

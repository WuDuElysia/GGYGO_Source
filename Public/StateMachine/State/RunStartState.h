/**
 * @file RunStartState.h
 * @brief 跑步启动状态
 *
 * Idle → RunStart（有移动输入时进入）。
 * 播放启动动画（非循环），动画播完或最短时间过后 → RunLoop。
 * 启动期间松手 → RunEnd（直接播停止动画）。
 * 允许跳跃/闪避/攻击打断。
 */
#pragma once

#include "StateMachine/CharacterState.h"

class FRunStartState : public FCharacterState
{
public:
	FRunStartState()
		: FCharacterState(ECharacterStateType::RunStart)
	{
	}

	virtual void Enter(FRuntimeData& RuntimeData) override;
	virtual void Update(float DeltaTime, FRuntimeData& RuntimeData, FCharacterStateMachine& SM) override;
	virtual void Exit(FRuntimeData& RuntimeData) override;

private:
	float ElapsedTime = 0.f;
};

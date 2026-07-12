/**
 * @file InteractingState.h
 * @brief 交互状态（System 组）
 *
 * 角色与场景物体交互时的状态（开箱子、对话等）。
 * 属于 System 组，可与 Overlay 状态叠加。
 * 移动被限制（CantMove），攻击/闪避受限。
 */
#pragma once

#include "StateMachine/CharacterState.h"

class FInteractingState : public FCharacterState
{
public:

	explicit FInteractingState();

	virtual void Enter(FRuntimeData& RuntimeData) override;
	virtual void Update(float DeltaTime, FRuntimeData& RuntimeData, FGYGOStateManager& SM) override;
	virtual void Exit(FRuntimeData& RuntimeData) override;
};

#pragma once
#include "StateMachine/CharacterState.h"

/**
 * 移动状态（Walk/Run/Sprint 统一）
 * 速度上限由 MotionDriver 根据输入意图管理，
 * 动画由 AnimBP 用 BlendSpace（Phase 9）驱动。
 * 本状态只负责：ActionGranted > 闪避 > 攻击 > 松手→Idle
 */
class FMovingState : public FCharacterState
{
public:
	FMovingState()
		: FCharacterState(ECharacterStateType::Moving, EStateGroup::Locomotion)
	{
	}

	virtual void Update(float DeltaTime, FRuntimeData& RuntimeData, FGYGOStateManager& SM) override;
};

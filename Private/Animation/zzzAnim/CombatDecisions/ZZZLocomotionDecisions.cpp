/**
 * @file ZZZLocomotionDecisions.cpp
 * @brief ZZZ 动画 Locomotion 决策模块实现
 */

#include "Animation/zzzAnim/CombatDecisions/ZZZLocomotionDecisions.h"
#include "Animation/zzzAnim/ZZZAnimSnapshot.h"

void FZZZLocomotionDecisions::SetSnap(FZZZAnimSnapshot* InSnap)
{
	Snap = InSnap;
}

bool FZZZLocomotionDecisions::NotMoving_To_Conduit() const
{
	if (!Snap)
	{
		return false;
	}

	return Snap->bShouldMove
		&& Snap->CurrentState == ECharacterStateType::Moving;
}

bool FZZZLocomotionDecisions::Conduit_To_EnterMove() const
{
	return Snap
		&& !Snap->bSprintTrigger;
}

bool FZZZLocomotionDecisions::Conduit_To_Moving_Sprint() const
{
	if (!Snap || !Snap->bSprintTrigger)
	{
		return false;
	}

	// 该开关只负责本次 Conduit 分流，条件成立后立即消费。
	Snap->bSprintTrigger = false;
	return true;
}

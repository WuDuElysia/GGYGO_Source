/**
 * @file DetailDecisions.cpp
 * @brief Layer 4.1 + 4.2 Detail 决策模块实现
 */
#include "Animation/Decisions/DetailDecisions.h"
#include "Animation/GGYGOAnimInstance.h"   // FAnimSnapshot 完整定义
#include "Animation/LocomotionConfig.h"    // FLocomotionTuning, EMovementGait, EAnimFoot

// 降级默认配置（默认构造使用 NTE 实证默认值）
const FLocomotionTuning FDetailDecisions::DefaultTuning;

// ----------------------------------------------------------------------------

void FDetailDecisions::Init(const FLocomotionTuning* InTuning)
{
	Tuning = InTuning ? InTuning : &DefaultTuning;
}

void FDetailDecisions::SetSnap(const FAnimSnapshot* InSnap)
{
	Snap = InSnap;
}

// ----------------------------------------------------------------------------
// Layer 4.1 Detail Decision Functions
// ----------------------------------------------------------------------------

bool FDetailDecisions::Detail_EnterRun_To_Run() const
{
	// 无条件立即过渡
	return true;
}

bool FDetailDecisions::Detail_Walk_To_Run() const
{
	if (!Snap) return false;
	return Snap->DesiredGait >= EMovementGait::Run;
}

bool FDetailDecisions::Detail_Walk_To_WalkToRun() const
{
	if (!Snap) return false;
	return Snap->DesiredGait >= EMovementGait::Run;
}

bool FDetailDecisions::Detail_Run_To_TurnBack() const
{
	if (!Snap) return false;
	return FMath::Abs(Snap->MoveAngleDeg) > Tuning->TurnBackAngle;
}

bool FDetailDecisions::Detail_WalkToRun_To_Run() const
{
	if (!Snap) return false;
	return Snap->DesiredGait == EMovementGait::Walk;
}

bool FDetailDecisions::Detail_TurnBack_To_Run() const
{
	if (!Snap) return false;
	return FMath::Abs(Snap->MoveAngleDeg) <= Tuning->TurnBackAngle;
}

bool FDetailDecisions::Detail_Run_To_Walk() const
{
	if (!Snap) return false;
	return Snap->DesiredGait == EMovementGait::Walk;
}

// ----------------------------------------------------------------------------
// Layer 4.2 RunTurnBackState Decision Functions
// ----------------------------------------------------------------------------

bool FDetailDecisions::Detail_TurnBack_IsLeft() const
{
	if (!Snap) return false;
	return Snap->CurrentFoot == EAnimFoot::Left;
}

bool FDetailDecisions::Detail_TurnBack_IsRight() const
{
	if (!Snap) return false;
	return Snap->CurrentFoot == EAnimFoot::Right;
}

// ----------------------------------------------------------------------------
// Layer 4.2 Sprint Decision Functions
// ----------------------------------------------------------------------------

bool FDetailDecisions::Gait_To_Sprint() const
{
	if (!Snap) return false;
	return Snap->DesiredGait == EMovementGait::Sprint;
}

bool FDetailDecisions::Gait_Exit_Sprint() const
{
	if (!Snap) return false;
	return Snap->DesiredGait < EMovementGait::Sprint;
}

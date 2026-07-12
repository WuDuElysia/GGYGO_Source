/**
 * @file StopGaitDecisions.cpp
 * @brief Layer 4.3 StopGait 决策模块实现
 */
#include "Animation/Decisions/StopGaitDecisions.h"
#include "Animation/GGYGOAnimInstance.h"   // FAnimSnapshot 完整定义
#include "Animation/LocomotionConfig.h"    // EMovementGait

// ----------------------------------------------------------------------------

void FStopGaitDecisions::SetSnap(const FAnimSnapshot* InSnap)
{
	Snap = InSnap;
}

// ----------------------------------------------------------------------------
// Layer 4.3 StopGait Decision Functions
// ----------------------------------------------------------------------------

bool FStopGaitDecisions::StopGait_Is_Sprint() const
{
	if (!Snap) return false;
	return Snap->DesiredGait == EMovementGait::Sprint;
}

bool FStopGaitDecisions::StopGait_Is_Run() const
{
	if (!Snap) return false;
	return Snap->DesiredGait == EMovementGait::Run || Snap->DesiredGait == EMovementGait::None;
}

bool FStopGaitDecisions::StopGait_Is_Walk() const
{
	if (!Snap) return false;
	return Snap->DesiredGait == EMovementGait::Walk;
}

bool FStopGaitDecisions::StopGait_ForceRun_To_State() const
{
	if (!Snap) return false;
	return Snap->DesiredGait != EMovementGait::Run;
}

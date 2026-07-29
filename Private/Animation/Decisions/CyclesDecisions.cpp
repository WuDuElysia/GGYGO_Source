/**
 * @file CyclesDecisions.cpp
 * @brief Layer 5 LocomotionCycles 决策模块实现
 *
 * 实现 18 个决策函数：
 *   8 个 Cycles_*  — 左右脚循环入口 + StopRotation 触发/结束 + Conduit 脚分发
 *   5 个 MoveR_*   — 右脚循环内 Sprint/RunWalk 切换
 *   5 个 MoveL_*   — 左脚循环内 Sprint/RunWalk 切换（与 MoveR 对称）
 */
#include "Animation/Decisions/CyclesDecisions.h"
#include "Animation/NTEAnimInstance.h"   // FAnimSnapshot 完整定义
#include "Animation/LocomotionConfig.h"    // FLocomotionTuning, EMovementGait, EAnimFoot

// 降级默认配置（默认构造使用 NTE 实证默认值）
const FLocomotionTuning FCyclesDecisions::DefaultTuning;

// ----------------------------------------------------------------------------

void FCyclesDecisions::Init(const FLocomotionTuning* InTuning)
{
	Tuning = InTuning ? InTuning : &DefaultTuning;
}

void FCyclesDecisions::SetSnap(const FAnimSnapshot* InSnap)
{
	Snap = InSnap;
}

// ----------------------------------------------------------------------------
// LocomotionCycles（8 个决策函数）
// ----------------------------------------------------------------------------

bool FCyclesDecisions::Cycles_To_Left() const
{
	if (!Snap) return false;
	return Snap->CurrentFoot == EAnimFoot::Left;
}

bool FCyclesDecisions::Cycles_To_Right() const
{
	if (!Snap) return false;
	return Snap->CurrentFoot == EAnimFoot::Right;
}

bool FCyclesDecisions::Cycles_To_RunStopRotation() const
{
	if (!Snap) return false;
	return !Snap->bWantMove
		&& FMath::Abs(Snap->MoveAngleDeg) > Tuning->TurnBackAngle
		&& Snap->DesiredGait == EMovementGait::Run;
}

bool FCyclesDecisions::Cycles_To_StopRotation() const
{
	if (!Snap) return false;
	return !Snap->bWantMove
		&& FMath::Abs(Snap->MoveAngleDeg) > Tuning->TurnBackAngle;
}

bool FCyclesDecisions::Cycles_To_WalkStopRotation() const
{
	if (!Snap) return false;
	return !Snap->bWantMove
		&& FMath::Abs(Snap->MoveAngleDeg) > Tuning->TurnBackAngle
		&& Snap->DesiredGait == EMovementGait::Walk;
}

bool FCyclesDecisions::Cycles_StopRotation_Done() const
{
	if (!Snap) return false;
	return Snap->bWantMove || FMath::Abs(Snap->MoveAngleDeg) <= Tuning->TurnBackAngle;
}

bool FCyclesDecisions::Cycles_Conduit_IsLeft() const
{
	if (!Snap) return false;
	return Snap->CurrentFoot == EAnimFoot::Left;
}

bool FCyclesDecisions::Cycles_Conduit_IsRight() const
{
	if (!Snap) return false;
	return Snap->CurrentFoot == EAnimFoot::Right;
}

// ----------------------------------------------------------------------------
// MoveR（5 个决策函数，右脚循环内 Sprint/RunWalk 切换）
// ----------------------------------------------------------------------------

bool FCyclesDecisions::MoveR_Conduit_To_RunWalk() const
{
	if (!Snap) return false;
	return Snap->DesiredGait != EMovementGait::Sprint;
}

bool FCyclesDecisions::MoveR_Conduit_To_SprintToRunWalk() const
{
	if (!Snap) return false;
	return Snap->DesiredGait != EMovementGait::Sprint;
}

bool FCyclesDecisions::MoveR_RunWalk_To_Sprint() const
{
	if (!Snap) return false;
	return Snap->DesiredGait == EMovementGait::Sprint;
}

bool FCyclesDecisions::MoveR_Sprint_To_RunWalk() const
{
	if (!Snap) return false;
	return Snap->DesiredGait < EMovementGait::Sprint;
}

bool FCyclesDecisions::MoveR_SprintToRunWalk_To_RunWalk() const
{
	// AutoRule：无条件过渡
	return true;
}

// ----------------------------------------------------------------------------
// MoveL（5 个决策函数，左脚循环内 Sprint/RunWalk 切换，与 MoveR 对称）
// ----------------------------------------------------------------------------

bool FCyclesDecisions::MoveL_Conduit_To_RunWalk() const
{
	if (!Snap) return false;
	return Snap->DesiredGait != EMovementGait::Sprint;
}

bool FCyclesDecisions::MoveL_Conduit_To_SprintToRunWalk() const
{
	if (!Snap) return false;
	return Snap->DesiredGait != EMovementGait::Sprint;
}

bool FCyclesDecisions::MoveL_RunWalk_To_Sprint() const
{
	if (!Snap) return false;
	return Snap->DesiredGait == EMovementGait::Sprint;
}

bool FCyclesDecisions::MoveL_Sprint_To_RunWalk() const
{
	if (!Snap) return false;
	return Snap->DesiredGait < EMovementGait::Sprint;
}

bool FCyclesDecisions::MoveL_SprintToRunWalk_To_RunWalk() const
{
	// AutoRule：无条件过渡
	return true;
}

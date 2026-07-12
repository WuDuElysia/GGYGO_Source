/**
 * @file DirectionDecisions.cpp
 * @brief Layer 3.5 Direction Dispatcher 决策模块实现
 */
#include "Animation/Decisions/DirectionDecisions.h"
#include "Animation/GGYGOAnimInstance.h"   // FAnimSnapshot 完整定义
#include "Animation/LocomotionConfig.h"    // EMovementGait, EAnimFoot

// ----------------------------------------------------------------------------

void FDirectionDecisions::SetSnap(const FAnimSnapshot* InSnap)
{
	Snap = InSnap;
}

// ----------------------------------------------------------------------------
// Direction Dispatcher（方向四分区 + Variant + Exit）
// ----------------------------------------------------------------------------

bool FDirectionDecisions::Enter_Dir_Forward() const
{
	if (!Snap) return false;
	return FMath::Abs(Snap->MoveAngleDeg) <= 45.f;
}

bool FDirectionDecisions::Enter_Dir_Left() const
{
	if (!Snap) return false;
	return Snap->MoveAngleDeg >= -135.f && Snap->MoveAngleDeg < -45.f;
}

bool FDirectionDecisions::Enter_Dir_Right() const
{
	if (!Snap) return false;
	return Snap->MoveAngleDeg > 45.f && Snap->MoveAngleDeg <= 135.f;
}

bool FDirectionDecisions::Enter_Dir_Forward_Variant() const
{
	if (!Snap) return false;
	return FMath::Abs(Snap->MoveAngleDeg) <= 45.f;
}

bool FDirectionDecisions::Enter_Dir_Back() const
{
	if (!Snap) return false;
	return FMath::Abs(Snap->MoveAngleDeg) > 135.f;
}

bool FDirectionDecisions::Enter_Exit_Check() const
{
	// AutoRule equivalent — always true
	return true;
}

// ----------------------------------------------------------------------------
// Forward Gait 子路由
// ----------------------------------------------------------------------------

bool FDirectionDecisions::EnterForward_Is_Sprint() const
{
	if (!Snap) return false;
	return Snap->DesiredGait == EMovementGait::Sprint;
}

bool FDirectionDecisions::EnterForward_Is_Run() const
{
	if (!Snap) return false;
	return Snap->DesiredGait == EMovementGait::Run || Snap->DesiredGait == EMovementGait::None;
}

// ----------------------------------------------------------------------------
// Forward Variant Gait 子路由
// ----------------------------------------------------------------------------

bool FDirectionDecisions::EnterForwardVar_Is_Sprint() const
{
	if (!Snap) return false;
	return Snap->DesiredGait == EMovementGait::Sprint;
}

bool FDirectionDecisions::EnterForwardVar_Is_Run() const
{
	if (!Snap) return false;
	return Snap->DesiredGait == EMovementGait::Run || Snap->DesiredGait == EMovementGait::None;
}

// ----------------------------------------------------------------------------
// Enter Back 脚分派
// ----------------------------------------------------------------------------

bool FDirectionDecisions::EnterBack_L_To_R() const
{
	if (!Snap) return false;
	return Snap->CurrentFoot == EAnimFoot::Right;
}

bool FDirectionDecisions::EnterBack_R_To_L() const
{
	if (!Snap) return false;
	return Snap->CurrentFoot == EAnimFoot::Left;
}

// ----------------------------------------------------------------------------
// BackLeft 子机内部过渡（AutoRule equivalents）
// ----------------------------------------------------------------------------

bool FDirectionDecisions::BackLeft_Start_To_B() const
{
	// AutoRule equivalent — always true
	return true;
}

bool FDirectionDecisions::BackLeft_Start_To_B_Auto() const
{
	// AutoRule equivalent — always true
	return true;
}

bool FDirectionDecisions::BackLeft_B_To_Exit() const
{
	// AutoRule equivalent — always true
	return true;
}

// ----------------------------------------------------------------------------
// BackRight 子机内部过渡（AutoRule equivalents）
// ----------------------------------------------------------------------------

bool FDirectionDecisions::BackRight_Start_To_B() const
{
	// AutoRule equivalent — always true
	return true;
}

bool FDirectionDecisions::BackRight_Start_To_B_Auto() const
{
	// AutoRule equivalent — always true
	return true;
}

bool FDirectionDecisions::BackRight_B_To_Exit() const
{
	// AutoRule equivalent — always true
	return true;
}

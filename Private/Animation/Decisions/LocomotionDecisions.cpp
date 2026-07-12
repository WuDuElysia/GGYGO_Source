/**
 * @file LocomotionDecisions.cpp
 * @brief Layer 3 Locomotion 决策模块实现
 */
#include "Animation/Decisions/LocomotionDecisions.h"
#include "Animation/GGYGOAnimInstance.h"   // FAnimSnapshot 完整定义
#include "Animation/LocomotionConfig.h"    // FLocomotionTuning, EMovementGait, EAnimFoot

// 降级默认配置（默认构造使用 NTE 实证默认值）
const FLocomotionTuning FLocomotionDecisions::DefaultTuning;

// ----------------------------------------------------------------------------
// Init / SetSnap
// ----------------------------------------------------------------------------

void FLocomotionDecisions::Init(const FLocomotionTuning* InTuning, FCurveValueDelegate InCurveDelegate, FStateWeightDelegate InWeightDelegate)
{
	Tuning = InTuning ? InTuning : &DefaultTuning;
	CurveDelegate = MoveTemp(InCurveDelegate);
	WeightDelegate = MoveTemp(InWeightDelegate);
}

void FLocomotionDecisions::SetSnap(const FAnimSnapshot* InSnap)
{
	Snap = InSnap;
}

// ----------------------------------------------------------------------------
// Private Helpers
// ----------------------------------------------------------------------------

bool FLocomotionDecisions::GetCurveValueSafe(FName CurveName, float& OutValue) const
{
	OutValue = 0.f;
	if (!CurveDelegate) return false;
	return CurveDelegate(CurveName, OutValue);
}

float FLocomotionDecisions::GetStateWeightSafe(int32 MachineIndex, int32 StateIndex) const
{
	if (!WeightDelegate) return 0.f;
	return WeightDelegate(MachineIndex, StateIndex);
}

// ============================================================================
// Layer 3 LocomotionStates（7 个决策函数）
// ============================================================================

bool FLocomotionDecisions::Loco_NotMoving_To_Enter() const
{
	if (!Snap) return false;
	return Snap->bWantMove && Snap->bGrounded;
}

bool FLocomotionDecisions::Loco_Enter_To_Moving() const
{
	return true;
}

bool FLocomotionDecisions::Loco_Enter_To_NotMoving() const
{
	if (!Snap) return false;
	return !Snap->bWantMove;
}

bool FLocomotionDecisions::Loco_Moving_To_LeftStop() const
{
	if (!Snap) return false;
	return !Snap->bWantMove && Snap->Speed < Tuning->ThresholdToStop && Snap->CurrentFoot == EAnimFoot::Left;
}

bool FLocomotionDecisions::Loco_Moving_To_RightStop() const
{
	if (!Snap) return false;
	return !Snap->bWantMove && Snap->Speed < Tuning->ThresholdToStop && Snap->CurrentFoot == EAnimFoot::Right;
}

bool FLocomotionDecisions::Loco_Stop_To_Moving() const
{
	if (!Snap) return false;
	return Snap->bWantMove;
}

bool FLocomotionDecisions::Loco_Stop_To_NotMoving() const
{
	return true;
}

// ============================================================================
// Layer 3 LocomotionStatesMachine（43 个决策函数）
// ============================================================================

// ---- Conduit entry (#662-#664) ----

bool FLocomotionDecisions::Loco3_Conduit_To_Moving_Sprint() const
{
	if (!Snap) return false;
	float SprintCurve = 0.f;
	GetCurveValueSafe(FName("Sprint"), SprintCurve);
	return SprintCurve > 0.1f;
}

bool FLocomotionDecisions::Loco3_Conduit_To_Moving() const
{
	if (!Snap) return false;
	return Snap->bEntryMovingOrNotMoving || Snap->bSkillInterruptMove;
}

bool FLocomotionDecisions::Loco3_Conduit_To_NotMoving() const
{
	if (!Snap) return false;
	return !Snap->bEntryMovingOrNotMoving && !Snap->bSkillInterruptMove;
}

// ---- NotMoving transitions (#665-#668) ----

bool FLocomotionDecisions::Loco3_NotMoving_To_Moving_Auto() const
{
	return true;
}

bool FLocomotionDecisions::Loco3_NotMoving_To_Conduit5() const
{
	if (!Snap) return false;
	return Snap->bIsHasInStandIdlePose && Snap->bNotMovingToMoving;
}

bool FLocomotionDecisions::Loco3_NotMoving_To_Moving_Alt() const
{
	if (!Snap) return false;
	return Snap->bIsHasInStandIdlePose && Snap->bNotMovingToMoving;
}

bool FLocomotionDecisions::Loco3_NotMoving_To_Stop_MM() const
{
	if (!Snap) return false;
	return Snap->bNeedMotionMatching;
}

// ---- Moving transitions (#669-#671) ----

bool FLocomotionDecisions::Loco3_Moving_To_NotMoving_Auto() const
{
	return true;
}

bool FLocomotionDecisions::Loco3_Moving_To_NotMoving() const
{
	if (!Snap) return false;
	return Snap->bIsCanRunStop && Snap->bMovingToNotMoving;
}

bool FLocomotionDecisions::Loco3_Moving_To_Conduit1() const
{
	if (!Snap) return false;
	return Snap->bIsCanRunStop && Snap->bMovingToNotMoving && Snap->bSkillInterruptMove;
}

// ---- Stop transitions (#672-#674) ----

bool FLocomotionDecisions::Loco3_Stop_To_NotMoving_Auto1() const
{
	return true;
}

bool FLocomotionDecisions::Loco3_Stop_To_NotMoving_Auto2() const
{
	return true;
}

bool FLocomotionDecisions::Loco3_Stop_To_Conduit4() const
{
	if (!Snap) return false;
	return Snap->bIsPatrolMoveAnim || Snap->bIsPatrolState || Snap->bShouldMove;
}

// ---- Patrol cycle (#675-#681) ----

bool FLocomotionDecisions::Loco3_NotMoving1_To_Moving1_Patrol() const
{
	if (!Snap) return false;
	return Snap->bIsPatrolMoveAnim && Snap->bIsPatrolState;
}

bool FLocomotionDecisions::Loco3_NotMoving1_To_LeftStop1() const
{
	if (!Snap) return false;
	return Snap->bIsCanRunStop && !Snap->bIsPatrolMoveAnim && !Snap->bIsPatrolState && Snap->bIsLeftFootC;
}

bool FLocomotionDecisions::Loco3_NotMoving1_To_RightStop1() const
{
	if (!Snap) return false;
	return Snap->bIsCanRunStop && !Snap->bIsPatrolMoveAnim && !Snap->bIsPatrolState && !Snap->bIsLeftFootC;
}

bool FLocomotionDecisions::Loco3_NotMoving1_To_Moving1_Should() const
{
	if (!Snap) return false;
	return Snap->bIsPatrolState && Snap->bShouldMove;
}

bool FLocomotionDecisions::Loco3_Moving1_To_NotMoving1() const
{
	if (!Snap) return false;
	return Snap->bIsCanRunStop && Snap->bIsPatrolMoveAnim && Snap->bIsPatrolState;
}

bool FLocomotionDecisions::Loco3_Moving1_To_CanStop1() const
{
	if (!Snap) return false;
	return !(Snap->bIsPatrolMoveAnim && Snap->bIsPatrolState);
}

bool FLocomotionDecisions::Loco3_AutoRule_Fallback() const
{
	return true;
}

// ---- Conduit_2 (#682-#684) ----

bool FLocomotionDecisions::Loco3_Conduit2_To_Moving1_Sprint() const
{
	if (!Snap) return false;
	float SprintCurve = 0.f;
	GetCurveValueSafe(FName("Sprint"), SprintCurve);
	return SprintCurve > 0.1f;
}

bool FLocomotionDecisions::Loco3_Conduit2_To_Moving1() const
{
	if (!Snap) return false;
	float ToMovingCurve = 0.f;
	GetCurveValueSafe(FName("ToMoving"), ToMovingCurve);
	float SprintTurnBackCurve = 0.f;
	GetCurveValueSafe(FName("sprintturnback"), SprintTurnBackCurve);
	return (Snap->VelocityLength > 1.f && Snap->bShouldMove)
		|| (Snap->VelocityLength > 1.f && ToMovingCurve != 0.f)
		|| SprintTurnBackCurve != 0.f;
}

bool FLocomotionDecisions::Loco3_Conduit2_To_NotMoving1() const
{
	return !Loco3_Conduit2_To_Moving1();
}

// ---- Stop_1 (#685-#687) ----

bool FLocomotionDecisions::Loco3_LeftStop1_To_NotMoving1_Auto() const
{
	return true;
}

bool FLocomotionDecisions::Loco3_Stop1_To_Moving1_Resume() const
{
	if (!Snap) return false;
	return Snap->bIsPatrolMoveAnim || Snap->bIsPatrolState || Snap->bShouldMove;
}

bool FLocomotionDecisions::Loco3_RightStop1_To_NotMoving1_Auto() const
{
	return true;
}

// ---- CanStop_1 (#688-#689) ----

bool FLocomotionDecisions::Loco3_CanStop1_To_Conduit1_1_1() const
{
	if (!Snap) return false;
	return FMath::Abs(Snap->LastInputDirectionAngle) < 5.f;
}

bool FLocomotionDecisions::Loco3_CanStop1_To_Conduit1_2() const
{
	if (!Snap) return false;
	return FMath::Abs(Snap->LastInputDirectionAngle) < 5.f;
}

// ---- StopConduit (#690-#691) ----

bool FLocomotionDecisions::Loco3_StopConduit_NoSprint() const
{
	if (!Snap) return false;
	float SprintCurve = 0.f;
	GetCurveValueSafe(FName("Sprint"), SprintCurve);
	return SprintCurve <= 0.1f;
}

bool FLocomotionDecisions::Loco3_StopConduit_Sprint() const
{
	if (!Snap) return false;
	float SprintCurve = 0.f;
	GetCurveValueSafe(FName("Sprint"), SprintCurve);
	return SprintCurve > 0.1f;
}

// ---- Stop route (#692-#693) ----

bool FLocomotionDecisions::Loco3_Conduit1_To_Stop_A() const
{
	return true;
}

bool FLocomotionDecisions::Loco3_Conduit1_To_Stop_B() const
{
	return true;
}

// ---- EnterMoveState (#694-#695) ----

bool FLocomotionDecisions::Loco3_EnterMove_To_Moving() const
{
	if (!Snap) return false;
	float ExitCurve = 0.f;
	GetCurveValueSafe(FName("ExitEnterMoveStateCurve"), ExitCurve);
	return ExitCurve != 0.f || Snap->Gait != EMovementGait::Run;
}

bool FLocomotionDecisions::Loco3_EnterMove_To_Conduit1() const
{
	if (!Snap) return false;
	return Snap->bMovingToNotMoving && Snap->bIsCanRunStop;
}

// ---- Conduit_3 (#696-#697) ----

bool FLocomotionDecisions::Loco3_Conduit3_To_Moving() const
{
	if (!Snap) return false;
	return !(Snap->bIsCanEnterMoveState && Snap->Gait == EMovementGait::Run && !Snap->bIsIgnoreMoveInput && !Snap->bIsHoldingHands);
}

bool FLocomotionDecisions::Loco3_Conduit3_To_EnterState() const
{
	if (!Snap) return false;
	return Snap->bIsCanEnterMoveState && Snap->Gait == EMovementGait::Run && !Snap->bIsIgnoreMoveInput && !Snap->bIsHoldingHands;
}

// ---- EnterState (#698) ----

bool FLocomotionDecisions::Loco3_EnterState_To_EnterMove() const
{
	if (!Snap) return false;
	return GetStateWeightSafe(3, 0) >= 1.f;
}

// ---- Conduit_5 (#699-#701) ----

bool FLocomotionDecisions::Loco3_Conduit5_To_Conduit3() const
{
	if (!Snap) return false;
	return Snap->Gait != EMovementGait::Walk;
}

bool FLocomotionDecisions::Loco3_Conduit5_To_Moving_Walk() const
{
	if (!Snap) return false;
	return Snap->Gait == EMovementGait::Walk && Snap->bIsHoldingHands;
}

bool FLocomotionDecisions::Loco3_Conduit5_To_EnterWalk() const
{
	if (!Snap) return false;
	return Snap->Gait == EMovementGait::Walk && !Snap->bIsHoldingHands;
}

// ---- EnterWalk (#702) ----

bool FLocomotionDecisions::Loco3_EnterWalk_To_Moving() const
{
	if (!Snap) return false;
	return GetStateWeightSafe(3, 1) >= 1.f;
}

// ---- Conduit_4 (#703-#704) ----

bool FLocomotionDecisions::Loco3_Conduit4_To_EnterState() const
{
	if (!Snap) return false;
	return Snap->bIsCanEnterMoveState && Snap->Gait == EMovementGait::Run && !Snap->bIsSprintStop && !Snap->bIsHoldingHands && Snap->bIsHasInStandIdlePose;
}

bool FLocomotionDecisions::Loco3_Conduit4_To_Moving() const
{
	if (!Snap) return false;
	return !(Snap->bIsCanEnterMoveState && Snap->Gait == EMovementGait::Run && !Snap->bIsSprintStop && !Snap->bIsHoldingHands && Snap->bIsHasInStandIdlePose);
}

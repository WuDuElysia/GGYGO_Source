/**
 * @file LocomotionDecisions.cpp
 * @brief Layer 3 Locomotion 决策模块实现
 */
#include "Animation/Decisions/LocomotionDecisions.h"
#include "Animation/NTEAnimInstance.h"   // FAnimSnapshot 完整定义
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
	return !Snap->bWantMove && Snap->CurrentFoot == EAnimFoot::Left;
}

bool FLocomotionDecisions::Loco_Moving_To_RightStop() const
{
	if (!Snap) return false;
	return !Snap->bWantMove && Snap->CurrentFoot == EAnimFoot::Right;
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
	// 直接进入移动（不走起步动画）的兜底：仅当"想动但待机姿势尚未稳定"时用，
	// 避免抢占正常的起步路径 NotMoving→Conduit_5→Enter*（那条要求 bIsHasInStandIdlePose）。
	// 待机稳定后（bIsHasInStandIdlePose=true）本条为假，交由 Conduit_5 走起步动画。
	if (!Snap) return false;
	return Snap->bWantMove && !Snap->bIsHasInStandIdlePose;
}

bool FLocomotionDecisions::Loco3_NotMoving_To_Conduit5() const
{
	if (!Snap) return false;
	return Snap->bIsHasInStandIdlePose && Snap->bNotMovingToMoving;
}

bool FLocomotionDecisions::Loco3_NotMoving_To_Moving_Alt() const
{
	// 直连 NotMoving→Moving（不走起步）。原条件与 #666 NotMoving→Conduit_5 完全相同，
	// 会和起步路径互相抢占（优先级高就跳过起步直接进 Moving）。
	// 现禁用此直连：所有起步统一走 Conduit_5（它再分发到 run 起步 / EnterWalk / 直接 Moving）。
	return false;
}

bool FLocomotionDecisions::Loco3_NotMoving_To_Stop_MM() const
{
	if (!Snap) return false;
	return Snap->bNeedMotionMatching;
}

// ---- Moving transitions (#669-#671) ----

bool FLocomotionDecisions::Loco3_Moving_To_NotMoving_Auto() const
{
	// 直接回待机（不播停步动画）的兜底：仅当"想停但并非从移动中急停"时用
	// （bIsCanRunStop=false，即上一帧未在移动的边缘情况）。正常停步走下方 #671→Stop 播停步动画。
	if (!Snap) return true;   // 无快照时安全退回待机
	return !Snap->bWantMove && !Snap->bIsCanRunStop;
}

bool FLocomotionDecisions::Loco3_Moving_To_NotMoving() const
{
	// 与 #669 同为"直接回待机"兜底路径；正常停步不走这里（交给 #671→Stop）。
	if (!Snap) return false;
	return !Snap->bWantMove && !Snap->bIsCanRunStop;
}

bool FLocomotionDecisions::Loco3_Moving_To_Conduit1() const
{
	// 正常停步主路径：移动中松开输入 → 进 Conduit_1 → Stop（播停步动画）。
	// 原条件额外要求 bSkillInterruptMove（玩法标志，恒 false），导致普通停步永远走不到 Stop、
	// 没有停步动画；现改为"有移动意图撤销且处于可跑停状态"即进 Stop。
	if (!Snap) return false;
	return !Snap->bWantMove && Snap->bIsCanRunStop;
}

// ---- Stop transitions (#672-#674) ----

bool FLocomotionDecisions::Loco3_Stop_To_NotMoving_Auto1() const
{
	// 主循环 Stop → NotMoving：停步动画播完才走。原 return true 会一进 Stop 就秒过渡、动画被跳过。
	if (!Snap) return true;
	return Snap->bStopFinished;
}

bool FLocomotionDecisions::Loco3_Stop_To_NotMoving_Auto2() const
{
	// 同 Auto1：停步动画播完才回待机。
	if (!Snap) return true;
	return Snap->bStopFinished;
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
	// 巡逻左脚停步 → 待机：停步动画播完才走（原 return true 会秒过渡）。
	if (!Snap) return true;
	return Snap->bStopFinished;
}

bool FLocomotionDecisions::Loco3_Stop1_To_Moving1_Resume() const
{
	if (!Snap) return false;
	return Snap->bIsPatrolMoveAnim || Snap->bIsPatrolState || Snap->bShouldMove;
}

bool FLocomotionDecisions::Loco3_RightStop1_To_NotMoving1_Auto() const
{
	// 巡逻右脚停步 → 待机：停步动画播完才走（原 return true 会秒过渡）。
	if (!Snap) return true;
	return Snap->bStopFinished;
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
	// 起步动画播完 → 进移动循环。原依赖动画曲线 ExitEnterMoveStateCurve，但该曲线被 FModel 剥掉了
	// （恒 0）→ Run 步态永远卡在 EnterMoveState。改用 bEnterFinished（起步计时器达到起步动画时长）。
	// 非 Run 步态不会进到 EnterMoveState（此处保留 Gait!=Run 作即时兜底）。
	if (!Snap) return true;
	return Snap->bEnterFinished || Snap->Gait != EMovementGait::Run;
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
	// EnterState 是 0 时长中转态（Conduit_3→EnterState 也是 0s），进入后应立即转到 EnterMoveState。
	// 原用 GetStateWeightSafe(3,0)>=1 判定，但机器索引 3 是错误的硬编码 → 永远取不到权重 →
	// 永远进不了 EnterMoveState。改为立即通过（中转态本就无动画、无需等待）。
	return true;
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
	// 走路无专门起步动画，EnterWalk 直接播 walk 循环即可立即进 Moving（同样播 walk BS）。
	// 原用 GetStateWeightSafe(3,1)>=1（机器索引硬编码错误）→ 永远为假 → 卡在 EnterWalk。改为立即通过。
	return true;
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

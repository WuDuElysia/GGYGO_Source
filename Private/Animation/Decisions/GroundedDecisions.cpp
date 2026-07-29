/**
 * @file GroundedDecisions.cpp
 * @brief Layer 2 Grounded 决策模块实现
 */
#include "Animation/Decisions/GroundedDecisions.h"
#include "Animation/NTEAnimInstance.h"   // FAnimSnapshot 完整定义
#include "Animation/LocomotionConfig.h"    // FLocomotionTuning 完整定义

// 降级默认配置（默认构造使用 NTE 实证默认值）
const FLocomotionTuning FGroundedDecisions::DefaultTuning;

// ----------------------------------------------------------------------------

void FGroundedDecisions::Init(const FLocomotionTuning* InTuning,
                              FAnimTimeRemainingDelegate InAnimTimeDelegate)
{
	Tuning = InTuning ? InTuning : &DefaultTuning;
	AnimTimeDelegate = MoveTemp(InAnimTimeDelegate);
}

void FGroundedDecisions::SetSnap(const FAnimSnapshot* InSnap)
{
	Snap = InSnap;
}

// ----------------------------------------------------------------------------
// AnimTime 安全包装
// ----------------------------------------------------------------------------

float FGroundedDecisions::GetAnimTimeRemainingSafe(int32 MachineIndex, int32 StateIndex) const
{
	// 代理未绑定时返回大哨兵值，使 `==0` 与 `<0.2` 条件均判 false（阻断过渡，安全降级）
	if (!AnimTimeDelegate) return TNumericLimits<float>::Max();
	return AnimTimeDelegate(MachineIndex, StateIndex);
}

// ----------------------------------------------------------------------------
// Layer 2 Decision Functions
// ----------------------------------------------------------------------------

bool FGroundedDecisions::Grounded_Just_Landed() const
{
	if (!Snap) return false;
	return Snap->bJustLanded && Snap->LandImpactSpeed > Tuning->JumpLandedThreshold;
}

// ----------------------------------------------------------------------------
// Entry 分派（#468–#477）
// ----------------------------------------------------------------------------

bool FGroundedDecisions::MG_Entry_To_VaultContinue() const
{
	if (!Snap) return false;
	// #468：进入翻越继续（Vault 子状态非 0 且可停留地面）
	return (Snap->GroundAnimState == 0x8) && (Snap->VaultSubState != 0x0) && Snap->bCanStayingTheGround;
}

bool FGroundedDecisions::MG_Entry_To_Conduit() const
{
	if (!Snap) return false;
	// #469：Conduit 分派入口，恒真
	return true;
}

bool FGroundedDecisions::MG_Entry_To_FromRoll() const
{
	if (!Snap) return false;
	// #470：翻滚落地
	return Snap->GroundAnimState == 0x7;
}

bool FGroundedDecisions::MG_Entry_To_LandedMobile() const
{
	if (!Snap) return false;
	// #471：落地且有移动意图
	return (Snap->GroundAnimState == 0x1) && Snap->bShouldMove;
}

bool FGroundedDecisions::MG_Entry_To_Landed() const
{
	if (!Snap) return false;
	// #472：落地静止（无移动意图、未播放蒙太奇、非滑翔落地）
	return (Snap->GroundAnimState == 0x1) && !Snap->bShouldMove
	    && !Snap->bIsPlayingAnyMontage && !Snap->bIsGlidingLanded;
}

bool FGroundedDecisions::MG_Entry_To_MainGS() const
{
	if (!Snap) return false;
	// #473：主地面状态兜底出边，恒真（复用 ×10）
	return true;
}

bool FGroundedDecisions::MG_Entry_To_VinesOver() const
{
	if (!Snap) return false;
	// #474：藤蔓结束
	return Snap->GroundAnimState == 0xA;
}

bool FGroundedDecisions::MG_Entry_To_RunOnWallsOver() const
{
	if (!Snap) return false;
	// #475：跑墙结束
	return Snap->GroundAnimState == 0xB;
}

bool FGroundedDecisions::MG_Entry_To_LandedStationary() const
{
	if (!Snap) return false;
	// #476：滑翔落地静止（水平速度 < 10 且落地）
	return Snap->bIsGlidingLanded && (Snap->Velocity2DLength < 10.f) && (Snap->GroundAnimState == 0x1);
}

bool FGroundedDecisions::MG_Entry_To_SprintVinesOver() const
{
	if (!Snap) return false;
	// #477：冲刺藤蔓结束
	return Snap->GroundAnimState == 0xC;
}

// ----------------------------------------------------------------------------
// FromRoll（#478–#479）
// ----------------------------------------------------------------------------

bool FGroundedDecisions::MG_FromRoll_To_MainGS() const
{
	if (!Snap) return false;
	// #478：兜底出边，恒真
	return true;
}

bool FGroundedDecisions::MG_FromRoll_To_RollToRun() const
{
	if (!Snap) return false;
	// #479：翻滚动画播完（剩余时间为 0）转 RollToRun
	return GetAnimTimeRemainingSafe(410, 2) == 0.f;
}

// ----------------------------------------------------------------------------
// LandedStationary（#480–#483）
// ----------------------------------------------------------------------------

bool FGroundedDecisions::MG_LandedStat_To_MainGS_Auto() const
{
	if (!Snap) return false;
	// #480：AutoRule 组合，恒真
	return true;
}

bool FGroundedDecisions::MG_LandedStat_To_StatToMove() const
{
	if (!Snap) return false;
	// #481：水平速度达到 10 转为移动
	return Snap->Velocity2DLength >= 10.f;
}

bool FGroundedDecisions::MG_LandedStat_To_LandedMob() const
{
	if (!Snap) return false;
	// #482：落地移动（非滑翔落地）
	return (Snap->GroundAnimState == 0x1) && (Snap->Velocity2DLength > 10.f) && !Snap->bIsGlidingLanded;
}

bool FGroundedDecisions::MG_LandedStat_To_MainGS() const
{
	if (!Snap) return false;
	// #483：落地移动（滑翔落地）
	return (Snap->GroundAnimState == 0x1) && (Snap->Velocity2DLength > 10.f) && Snap->bIsGlidingLanded;
}

// ----------------------------------------------------------------------------
// LandedMobile（#484–#485）
// ----------------------------------------------------------------------------

bool FGroundedDecisions::MG_LandedMob_To_MainGS_Auto() const
{
	if (!Snap) return false;
	// #484：AutoRule 组合，恒真
	return true;
}

bool FGroundedDecisions::MG_LandedMob_To_MainGS() const
{
	if (!Snap) return false;
	// #485：兜底出边，恒真
	return true;
}

// ----------------------------------------------------------------------------
// StatToMove（#486–#487）
// ----------------------------------------------------------------------------

bool FGroundedDecisions::MG_StatToMove_To_MainGS_Auto() const
{
	if (!Snap) return false;
	// #486：AutoRule 组合，恒真
	return true;
}

bool FGroundedDecisions::MG_StatToMove_To_MainGS() const
{
	if (!Snap) return false;
	// #487：无移动意图且过渡动画将播完（剩余时间 < 0.2）
	return !Snap->bShouldMove && (GetAnimTimeRemainingSafe(410, 5) < 0.2f);
}

// ----------------------------------------------------------------------------
// RollToRun（#488–#489）
// ----------------------------------------------------------------------------

bool FGroundedDecisions::MG_RollToRun_To_MainGS_Auto() const
{
	if (!Snap) return false;
	// #488：AutoRule 组合，恒真
	return true;
}

bool FGroundedDecisions::MG_RollToRun_To_MainGS() const
{
	if (!Snap) return false;
	// #489：翻越结束到停止（左或右）
	return Snap->VaultEndToStopL || Snap->VaultEndToStopR;
}

// ----------------------------------------------------------------------------
// Landed（#490–#491）
// ----------------------------------------------------------------------------

bool FGroundedDecisions::MG_Landed_To_LandedStat() const
{
	if (!Snap) return false;
	// #490：落地动画播完（剩余时间为 0）且无移动意图转静止
	return (GetAnimTimeRemainingSafe(410, 7) == 0.f) && !Snap->bShouldMove;
}

bool FGroundedDecisions::MG_Landed_To_StatToMove() const
{
	if (!Snap) return false;
	// #491：兜底出边，恒真
	return true;
}

// ----------------------------------------------------------------------------
// VaultContinue（#492）
// ----------------------------------------------------------------------------

bool FGroundedDecisions::MG_VaultCont_To_MainGS() const
{
	if (!Snap) return false;
	// #492：翻越结束（回归 Normal 或 Vault 子状态归零）
	return (Snap->GroundAnimState == 0x0) || (Snap->VaultSubState == 0x0);
}

// ----------------------------------------------------------------------------
// VinesOver（#493–#494）
// ----------------------------------------------------------------------------

bool FGroundedDecisions::MG_VinesOver_To_MainGS_Auto() const
{
	if (!Snap) return false;
	// #493：AutoRule 组合，恒真
	return true;
}

bool FGroundedDecisions::MG_VinesOver_To_MainGS() const
{
	if (!Snap) return false;
	// #494：藤蔓结束回归 Normal
	return Snap->GroundAnimState == 0x0;
}

// ----------------------------------------------------------------------------
// RunOnWallsOver（#495–#496）
// ----------------------------------------------------------------------------

bool FGroundedDecisions::MG_RunWalls_To_MainGS_Auto() const
{
	if (!Snap) return false;
	// #495：AutoRule 组合，恒真
	return true;
}

bool FGroundedDecisions::MG_RunWalls_To_MainGS() const
{
	if (!Snap) return false;
	// #496：跑墙结束回归 Normal 或落地
	return (Snap->GroundAnimState == 0x0) || (Snap->GroundAnimState == 0x1);
}

// ----------------------------------------------------------------------------
// Conduit（#497–#498）
// ----------------------------------------------------------------------------

bool FGroundedDecisions::MG_Conduit_To_LandedMob() const
{
	if (!Snap) return false;
	// #497：Conduit 分派出边，恒真
	return true;
}

bool FGroundedDecisions::MG_Conduit_To_Landed() const
{
	if (!Snap) return false;
	// #498：Conduit 分派出边，恒真
	return true;
}

// ----------------------------------------------------------------------------
// SprintVinesOver（#499–#500）
// ----------------------------------------------------------------------------

bool FGroundedDecisions::MG_SprintVines_To_MainGS() const
{
	if (!Snap) return false;
	// #499：冲刺藤蔓结束回归 Normal
	return Snap->GroundAnimState == 0x0;
}

bool FGroundedDecisions::MG_SprintVines_To_MainGS_Auto() const
{
	if (!Snap) return false;
	// #500：AutoRule 组合，恒真
	return true;
}

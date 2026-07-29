/**
 * @file MotionMatchDecisions.cpp
 * @brief 顶层辅助 MotionMatch 决策模块实现
 */
#include "Animation/Decisions/MotionMatchDecisions.h"
#include "Animation/NTEAnimInstance.h"  // FAnimSnapshot 完整定义

// ----------------------------------------------------------------------------

void FMotionMatchDecisions::SetSnap(const FAnimSnapshot* InSnap)
{
	Snap = InSnap;
}

// ----------------------------------------------------------------------------
// 顶层辅助 MM 决策函数（#44–#45）
// 判定语义严格对齐 NTE_05 附录 B.2.9 明细表。
// ----------------------------------------------------------------------------

bool FMotionMatchDecisions::MM_To_Base_Instant() const
{
	if (!Snap) return false;
	return Snap->bForceExitMotionMatching;  // #44
}

bool FMotionMatchDecisions::MM_To_Base_Smooth() const
{
	if (!Snap) return false;
	return Snap->bMotionMatchComplete || !Snap->bMotionMatchActive;  // #45
}

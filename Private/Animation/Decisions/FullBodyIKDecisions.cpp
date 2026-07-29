/**
 * @file FullBodyIKDecisions.cpp
 * @brief 顶层辅助 FullBodyIK 决策模块实现
 */
#include "Animation/Decisions/FullBodyIKDecisions.h"
#include "Animation/NTEAnimInstance.h"  // FAnimSnapshot 完整定义

// ----------------------------------------------------------------------------

void FFullBodyIKDecisions::SetSnap(const FAnimSnapshot* InSnap)
{
	Snap = InSnap;
}

// ----------------------------------------------------------------------------
// 顶层辅助 IK 决策函数（#64–#65）
// 判定语义严格对齐 NTE_05 附录 B.2.9 明细表。
// ----------------------------------------------------------------------------

bool FFullBodyIKDecisions::FullBodyIK_Activate() const
{
	if (!Snap) return false;
	return Snap->bFullBodyIKNeeded;  // #64
}

bool FFullBodyIKDecisions::FullBodyIK_Deactivate() const
{
	if (!Snap) return false;
	return !Snap->bFullBodyIKNeeded;  // #65
}

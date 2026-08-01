/**
 * @file ZZZAnimSnapshotCapture.cpp
 * @brief ZZZ 动画快照抓取器实现
 *
 * 从 Owner->GetRuntimeData()->AnimData 直读，不做二次拷贝。
 */

#include "Animation/zzzAnim/ZZZAnimSnapshotCapture.h"
#include "BaseCharacter.h"
#include "Data/Logic/RuntimeData.h"

void FZZZAnimSnapshotCapture::Capture(FZZZAnimSnapshot& OutSnap, ABaseCharacter* InOwner)
{
	OutSnap = FZZZAnimSnapshot();

	if (!InOwner) return;

	const FRuntimeData* RuntimeData = InOwner->GetRuntimeData();
	if (!RuntimeData) return;

	const FAnimRuntimeData& AnimData = RuntimeData->AnimData;

	// Locomotion
	OutSnap.Gait           = AnimData.Gait;
	OutSnap.bShouldMove    = AnimData.bShouldMove;
	OutSnap.bSprintTrigger = AnimData.bSprintTrigger;
	OutSnap.CurrentState   = AnimData.CurrentState;
	OutSnap.VelocityLength = AnimData.VelocityLength;

	// 通用
	OutSnap.bGrounded  = InOwner->IsGrounded();
	OutSnap.bBlockMove = false;  // 暂无来源
}

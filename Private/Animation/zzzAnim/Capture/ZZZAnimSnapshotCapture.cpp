/**
 * @file ZZZAnimSnapshotCapture.cpp
 * @brief ZZZ 动画快照抓取器实现
 *
 * 从 Owner->GetRuntimeData()->ZZZAnim 直读，不做二次拷贝。
 */

#include "Animation/zzzAnim/Capture/ZZZAnimSnapshotCapture.h"
#include "BaseCharacter.h"
#include "Data/Runtime/RuntimeData.h"

void FZZZAnimSnapshotCapture::Capture(FZZZAnimSnapshot& OutSnap, ABaseCharacter* InOwner)
{
	OutSnap = FZZZAnimSnapshot();

	if (!InOwner) return;

	const FRuntimeData* RuntimeData = InOwner->GetRuntimeData();
	if (!RuntimeData) return;

	const FZZZAnimRuntimeModel& AnimData = RuntimeData->ZZZAnim;

	// Locomotion
	OutSnap.Gait           = AnimData.Gait;
	OutSnap.bShouldMove    = AnimData.bShouldMove;
	OutSnap.AnimBlendX     = AnimData.AnimBlendX;
	OutSnap.AnimBlendY     = AnimData.AnimBlendY;
	OutSnap.CurrentState   = AnimData.CurrentState;
	OutSnap.VelocityLength = AnimData.VelocityLength;
	OutSnap.TurnBackPhase  = RuntimeData->Movement.TurnBack.Phase;

	// 反向输入点积仅作诊断保留：使用已由摄像机修正的世界移动方向，与角色当前水平前向计算。
	const FVector DesiredMoveDir = RuntimeData->Intent.DesiredWorldMoveDir.GetSafeNormal2D();
	const FVector ActorForward = InOwner->GetActorForwardVector().GetSafeNormal2D();
	if (!DesiredMoveDir.IsNearlyZero() && !ActorForward.IsNearlyZero())
	{
		OutSnap.InputForwardDot = FMath::Clamp(
			FVector::DotProduct(ActorForward, DesiredMoveDir),
			-1.0f,
			1.0f);
	}

	// 通用
	OutSnap.bGrounded  = InOwner->IsGrounded();
	OutSnap.bBlockMove = false;  // 暂无来源
}

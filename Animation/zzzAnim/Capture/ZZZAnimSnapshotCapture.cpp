/**
 * @file ZZZAnimSnapshotCapture.cpp
 * @brief ZZZ 动画快照抓取器实现
 *
 * 从 Owner->GetRuntimeData() 读取 ZZZAnim 和 RootMotion，不做二次拷贝。
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
	OutSnap.AnimCurveVelocity = RuntimeData->RootMotion.AnimCurveVelocity;
	OutSnap.AnimCurveVelocityDirection = RuntimeData->RootMotion.AnimCurveVelocityDirection;
	OutSnap.AnimCurveVelocityAngle = RuntimeData->RootMotion.AnimCurveAngle;
	OutSnap.CurrentState   = AnimData.CurrentState;
	OutSnap.VelocityLength = AnimData.VelocityLength;
	OutSnap.ActualVelocityDirection = AnimData.ActualVelocityDirection;
	OutSnap.ActualVelocityBlendX = AnimData.ActualVelocityBlendX;
	OutSnap.ActualVelocityBlendY = AnimData.ActualVelocityBlendY;
	OutSnap.ActualVelocityAngle = AnimData.ActualVelocityAngle;
	OutSnap.TurnBackPhase = RuntimeData->Movement.TurnBack.Phase;
	OutSnap.bCanYaw = RuntimeData->Movement.TurnBack.bCanYaw;
	OutSnap.bTurnBackSecondSegment = RuntimeData->Movement.TurnBack.bSecondSegment;

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

/**
 * @file ZZZAnimSnapshotCapture.cpp
 * @brief 动画快照抓取实现
 */
#include "Animation/zzzAnim/Capture/ZZZAnimSnapshotCapture.h"

#include "Animation/zzzAnim/Data/ZZZAnimSnapshot.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "GameFramework/Character.h"

void FZZZAnimSnapshotCapture::Capture(FZZZAnimSnapshot& OutSnap, const ACharacter* InOwner) const
{
	// 整体重置。见头文件对"为什么不做部分更新"的说明。
	OutSnap = FZZZAnimSnapshot();

	if (!InOwner)
	{
		return;
	}

	const UGGYGOCharacterMovementComponent* MoveComp =
		Cast<UGGYGOCharacterMovementComponent>(InOwner->GetCharacterMovement());
	if (!MoveComp)
	{
		// 不是项目 CMC（例如编辑器预览用的裸 Character）。
		// 保持全默认值，动画停在待机，不崩。
		return;
	}

	// ===== 步态与移动意图 =====
	OutSnap.Gait = MoveComp->GetResolvedGait();
	OutSnap.bShouldMove = MoveComp->HasMoveInput();
	OutSnap.bBlockMove = MoveComp->IsMovementBlockedByTag();
	OutSnap.bGrounded = MoveComp->IsMovingOnGround();

	// ===== 实际速度派生量 =====
	OutSnap.VelocityLength = MoveComp->GetHorizontalSpeed();
	OutSnap.ActualVelocityDirection = MoveComp->GetHorizontalVelocityDirection();
	OutSnap.ActualVelocityAngle = MoveComp->GetLocalVelocityAngle();
	MoveComp->GetLocalVelocityBlend(OutSnap.ActualVelocityBlendX, OutSnap.ActualVelocityBlendY);

	// BlendSpace 主输入用实际速度的分量。
	//
	// 平滑由 CMC 的加减速提供，这里不再额外插值 —— 两级平滑串联会让
	// 混合响应明显滞后于角色实际转向。
	OutSnap.AnimBlendX = OutSnap.ActualVelocityBlendX;
	OutSnap.AnimBlendY = OutSnap.ActualVelocityBlendY;

	// ===== 诊断用点积 =====
	const FVector MoveIntent = MoveComp->GetCurrentAcceleration().GetSafeNormal2D();
	const FVector ActorForward = InOwner->GetActorForwardVector().GetSafeNormal2D();
	if (!MoveIntent.IsNearlyZero() && !ActorForward.IsNearlyZero())
	{
		OutSnap.InputForwardDot = FMath::Clamp(
			FVector::DotProduct(ActorForward, MoveIntent),
			-1.0f,
			1.0f);
	}

	// ===== 曲线与 TurnBack =====
	// AnimCurveVelocity / AnimCurveVelocityDirection / AnimCurveVelocityAngle /
	// TurnBackPhase / bCanYaw / bTurnBackSecondSegment 保持构造默认值：
	// 它们的生产者依赖动画曲线采样，而曲线采样尚未接入 CMC。
	// 后果是转身状态不会被进入。
}

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
		// 不是项目 CMC（旧 ABaseCharacter，或编辑器预览用的裸 Character）。
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

	// BlendSpace 主输入暂时直接用实际速度的分量。
	//
	// 旧实现里 AnimBlendX/Y 是**输入方向**经 FInterpTo 平滑后的值，与实际速度不同：
	// 输入方向在起步第一帧就是满值，而实际速度要加速几帧才到位。
	// 那个平滑属于输入层职责（阶段 7 重建），在此之前用实际速度是最接近的替代 ——
	// CMC 的加减速本身就提供了平滑，只是响应比原来慢一点。
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
	// 阶段 6 在此填入 AnimCurveVelocity / AnimCurveVelocityDirection /
	// AnimCurveVelocityAngle / TurnBackPhase / bCanYaw / bTurnBackSecondSegment。
	// 现在保持构造默认值，转身状态因此不会被进入。
}

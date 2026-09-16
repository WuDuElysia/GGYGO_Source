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

	// ===== 曲线运动量 =====
	// 原样转发，不换算到角色当前朝向的局部空间 ——
	// AnimBP 里对照曲线编辑器调参时看到的应当是同一组数值。
	// 代价是转身时这组值的基准（动画段起点朝向）与角色当前朝向不一致，
	// 需要世界方向的消费方自己用入口朝向换算。
	const FGGYGOAnimCurveMotion& CurveMotion = MoveComp->GetCurveMotion();
	OutSnap.AnimCurveVelocity = CurveMotion.Velocity;
	OutSnap.AnimCurveVelocityDirection = CurveMotion.Direction;
	OutSnap.AnimCurveVelocityAngle = CurveMotion.DirectionAngle;

	// ===== 转身 =====
	OutSnap.TurnBackPhase = MoveComp->GetTurnBackPhase();
	OutSnap.bTurnBackRunOut = MoveComp->IsTurnBackRunOut();
}

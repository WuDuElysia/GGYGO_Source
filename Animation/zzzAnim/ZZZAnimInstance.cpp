/**
 * @file ZZZAnimInstance.cpp
 * @brief ZZZ 动画决策层实现
 */

#include "Animation/zzzAnim/ZZZAnimInstance.h"
#include "Animation/zzzAnim/ZZZAnimLog.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"

// ============================================================================
// AnimInstance 生命周期
// ============================================================================

void UZZZAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
	Owner = Cast<ACharacter>(TryGetPawnOwner());
}

void UZZZAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);
	RefreshDecisionContext(DeltaSeconds);
}

void UZZZAnimInstance::AnimNotify_CanYaw()
{
	// 把控制权交还玩家输入的时机由动画决定，而不是由代码里的固定秒数决定 ——
	// 换一版转身动画时不必回来改配置。
	if (const ACharacter* Character = Owner.Get())
	{
		if (UGGYGOCharacterMovementComponent* MoveComp =
			Cast<UGGYGOCharacterMovementComponent>(Character->GetCharacterMovement()))
		{
			MoveComp->NotifyCanYaw();
		}
	}
}

void UZZZAnimInstance::RefreshDecisionContext(float DeltaSeconds)
{
	// 固定顺序：抓取快照 → 注入上下文 → 由 C++ 同步 Moving 子状态 → 推进表现记忆。
	SnapshotCapture.Capture(Snap, Owner.Get());
	AnimBlendX = Snap.AnimBlendX;
	AnimBlendY = Snap.AnimBlendY;
	AnimCurveVelocity = Snap.AnimCurveVelocity;
	AnimCurveVelocityDirection = Snap.AnimCurveVelocityDirection;
	AnimCurveVelocityAngle = Snap.AnimCurveVelocityAngle;
	ActualVelocityDirection = Snap.ActualVelocityDirection;
	ActualVelocityBlendX = Snap.ActualVelocityBlendX;
	ActualVelocityBlendY = Snap.ActualVelocityBlendY;
	ActualVelocityAngle = Snap.ActualVelocityAngle;
	bCanYaw = Snap.bCanYaw;
	bTurnBackSecondSegment = Snap.bTurnBackSecondSegment;

	FZZZAnimWriteContext WriteContext;
	WriteContext.Snap = &Snap;
	WriteContext.Tuning = &Tuning;
	WriteContext.Memory = &StateMemory;

	LocomotionDecisions.SetContext(WriteContext.ToRead());
	LocomotionEvents.SetContext(WriteContext);
	LocomotionEvents.SynchronizeMovingSubState();
	LocomotionEvents.AdvanceGaitBlend(DeltaSeconds);

#if !UE_BUILD_SHIPPING
	// TurnBack 诊断。全部字段取自快照，不回头读移动层 ——
	// 快照之外再取一次值，两者可能来自不同时刻，日志就会自相矛盾。
	if (Snap.TurnBackPhase != EGGYGOTurnBackPhase::None
		|| StateMemory.MovingSubState == EZZZAnimMovingSubState::TurnBack)
	{
		UE_LOG(LogZZZAnim, Log,
			TEXT("[TurnBack][Snapshot] Phase=%d SecondSegment=%d SubState=%d Gait=%d ShouldMove=%d Grounded=%d BlockMove=%d InputForwardDot=%.3f Velocity=%.2f"),
			static_cast<uint8>(Snap.TurnBackPhase),
			Snap.bTurnBackSecondSegment ? 1 : 0,
			static_cast<uint8>(StateMemory.MovingSubState),
			static_cast<uint8>(Snap.Gait),
			Snap.bShouldMove ? 1 : 0,
			Snap.bGrounded ? 1 : 0,
			Snap.bBlockMove ? 1 : 0,
			Snap.InputForwardDot,
			Snap.VelocityLength);
	}
#endif
}

void UZZZAnimInstance::NativeThreadSafeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeThreadSafeUpdateAnimation(DeltaSeconds);
}

// ============================================================================
// Locomotion 过渡决策
// ============================================================================

bool UZZZAnimInstance::Locomotion_NotMoving_To_Conduit() const
{
	return LocomotionDecisions.NotMoving_To_Conduit();
}

bool UZZZAnimInstance::Locomotion_Stop_To_Conduit() const
{
	return LocomotionDecisions.Stop_To_Conduit();
}

bool UZZZAnimInstance::Locomotion_Conduit_To_EnterMove() const
{
	return LocomotionDecisions.Conduit_To_EnterMove();
}

bool UZZZAnimInstance::Locomotion_Conduit_To_Moving_Direct() const
{
	return LocomotionDecisions.Conduit_To_Moving_Direct();
}

bool UZZZAnimInstance::Locomotion_Moving_To_Stop() const
{
	return LocomotionDecisions.ShouldExitMoving();
}

bool UZZZAnimInstance::Locomotion_EnterMove_To_Stop() const
{
	return LocomotionDecisions.ShouldStopMoving();
}

bool UZZZAnimInstance::Locomotion_WalkRun_To_TurnBack() const
{
	return LocomotionDecisions.WalkRun_To_TurnBack();
}

// ============================================================================
// 配表查询
// ============================================================================

UAnimSequence* UZZZAnimInstance::GetSeqByKey(FName Key) const
{
	return AnimSet.Sequences.FindRef(Key);
}

UBlendSpace* UZZZAnimInstance::GetBlendSpaceByKey(FName Key) const
{
	return AnimSet.BlendSpaces.FindRef(Key);
}

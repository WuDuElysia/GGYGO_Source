/**
 * @file ZZZAnimInstance.cpp
 * @brief ZZZ 动画决策层实现
 */

#include "Animation/zzzAnim/ZZZAnimInstance.h"
#include "Animation/zzzAnim/ZZZAnimLog.h"
#include "BaseCharacter.h"
#include "Components/SkeletalMeshComponent.h"
#include "Data/Runtime/RuntimeData.h"

// ============================================================================
// AnimInstance 生命周期
// ============================================================================

void UZZZAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
	Owner = Cast<ABaseCharacter>(TryGetPawnOwner());
}

void UZZZAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);

	if (bDrivenByPipeline)
	{
		bDrivenByPipeline = false;
		return;
	}

	// 降级路径：管线未驱动（编辑器预览等场景）
	RefreshDecisionContext(DeltaSeconds);
}

void UZZZAnimInstance::PipelineDrive(float DeltaSeconds)
{
	RefreshDecisionContext(DeltaSeconds);
	bDrivenByPipeline = true;
}

void UZZZAnimInstance::AnimNotify_CanYaw()
{
	if (ABaseCharacter* Character = Owner.Get())
	{
		Character->NotifyCanYaw();
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

	const ABaseCharacter* Character = Owner.Get();
	const FRuntimeData* RuntimeData = Character
		? Character->GetRuntimeData()
		: nullptr;

	FZZZAnimWriteContext WriteContext;
	WriteContext.Snap = &Snap;
	WriteContext.Tuning = &Tuning;
	WriteContext.Memory = &StateMemory;

	LocomotionDecisions.SetContext(WriteContext.ToRead());
	LocomotionEvents.SetContext(WriteContext);
	LocomotionEvents.SynchronizeMovingSubState();
	LocomotionEvents.AdvanceGaitBlend(DeltaSeconds);

	if (Snap.TurnBackPhase != ETurnBackPhase::None
		|| StateMemory.MovingSubState == EZZZAnimMovingSubState::TurnBack)
	{
		const FVector DesiredMoveDir = RuntimeData
			? RuntimeData->Intent.DesiredWorldMoveDir.GetSafeNormal2D()
			: FVector::ZeroVector;
		const FVector ActorForward = Character
			? Character->GetActorForwardVector().GetSafeNormal2D()
			: FVector::ZeroVector;
		const float ActorDesiredDot = !DesiredMoveDir.IsNearlyZero()
			&& !ActorForward.IsNearlyZero()
			? FVector::DotProduct(ActorForward, DesiredMoveDir)
			: 1.0f;
		const float CurrentVelocity = RuntimeData
			? RuntimeData->Movement.CurrentSpeed
			: 0.0f;

		UE_LOG(LogZZZAnim, Log,
			TEXT("[TurnBack][Snapshot] Phase=%d SecondSegment=%d SubState=%d State=%d Gait=%d ShouldMove=%d InputForwardDot=%.3f ActorDesiredDot=%.3f Velocity=%.2f"),
			static_cast<uint8>(Snap.TurnBackPhase),
			Snap.bTurnBackSecondSegment ? 1 : 0,
			static_cast<uint8>(StateMemory.MovingSubState),
			static_cast<uint8>(Snap.CurrentState),
			static_cast<uint8>(Snap.Gait),
			Snap.bShouldMove ? 1 : 0,
			Snap.InputForwardDot,
			ActorDesiredDot,
			CurrentVelocity);
	}
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

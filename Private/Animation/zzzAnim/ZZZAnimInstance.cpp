/**
 * @file ZZZAnimInstance.cpp
 * @brief ZZZ 动画决策层实现
 */

#include "Animation/zzzAnim/ZZZAnimInstance.h"
#include "BaseCharacter.h"

DEFINE_LOG_CATEGORY_STATIC(LogZZZAnim, Log, All);

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
	SnapshotCapture.Capture(Snap, Owner.Get());
	LocomotionDecisions.SetSnap(&Snap);
}

void UZZZAnimInstance::PipelineDrive()
{
	SnapshotCapture.Capture(Snap, Owner.Get());
	LocomotionDecisions.SetSnap(&Snap);
	bDrivenByPipeline = true;
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

bool UZZZAnimInstance::Locomotion_Conduit_To_EnterMove() const
{
	return LocomotionDecisions.Conduit_To_EnterMove();
}

bool UZZZAnimInstance::Locomotion_Conduit_To_Moving_Sprint() const
{
	return LocomotionDecisions.Conduit_To_Moving_Sprint();
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

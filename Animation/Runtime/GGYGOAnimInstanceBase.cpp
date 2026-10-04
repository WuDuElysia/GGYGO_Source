/**
 * @file GGYGOAnimInstanceBase.cpp
 * @brief 项目通用动画实例基类实现
 */
#include "Animation/Runtime/GGYGOAnimInstanceBase.h"

#include "GameFramework/Character.h"

void UGGYGOAnimInstanceBase::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
	ResetAnimationLifecycleState();

	ACharacter* CurrentOwner = Cast<ACharacter>(TryGetPawnOwner());
	CharacterOwner = CurrentOwner;
	bHadCharacterOwner = CurrentOwner != nullptr;
	RefreshAnimationStateFrame();
}

void UGGYGOAnimInstanceBase::NativeUninitializeAnimation()
{
	ResetAnimationLifecycleState();
	Super::NativeUninitializeAnimation();
}

void UGGYGOAnimInstanceBase::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);

	ACharacter* CurrentOwner = Cast<ACharacter>(TryGetPawnOwner());
	const bool bPreviousOwnerExpired = bHadCharacterOwner && !CharacterOwner.IsValid();
	if (CharacterOwner.Get() != CurrentOwner || bPreviousOwnerExpired)
	{
		ResetAnimationLifecycleState();
		CharacterOwner = CurrentOwner;
		bHadCharacterOwner = CurrentOwner != nullptr;
	}

	RefreshAnimationStateFrame();
}

bool UGGYGOAnimInstanceBase::HasAnimationStateTag(FGameplayTag Tag) const
{
	return Tag.IsValid() && AnimationState.OwnedStateTags.HasTag(Tag);
}

bool UGGYGOAnimInstanceBase::IsAnimationRunGait() const
{
	return AnimationState.Gait == EGGYGOGait::Run;
}

bool UGGYGOAnimInstanceBase::IsTurnBackCurveDriven() const
{
	return AnimationState.TurnBackPhase == EGGYGOTurnBackPhase::Turning
		|| AnimationState.TurnBackPhase == EGGYGOTurnBackPhase::Braking;
}

void UGGYGOAnimInstanceBase::OnAnimationLifecycleReset()
{
}

void UGGYGOAnimInstanceBase::ResetAnimationLifecycleState()
{
	CharacterOwner.Reset();
	bHadCharacterOwner = false;
	AnimationState = {};
	AnimationDebug = {};
	OnAnimationLifecycleReset();
}

void UGGYGOAnimInstanceBase::RefreshAnimationStateFrame()
{
	StateCapture.Capture(AnimationState, AnimationDebug, CharacterOwner.Get());
}

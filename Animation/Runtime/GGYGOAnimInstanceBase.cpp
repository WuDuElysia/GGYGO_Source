/**
 * @file GGYGOAnimInstanceBase.cpp
 * @brief 项目通用动画实例基类实现
 */
#include "Animation/Runtime/GGYGOAnimInstanceBase.h"

#include "GameFramework/Character.h"

void UGGYGOAnimInstanceBase::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
	CharacterOwner = Cast<ACharacter>(TryGetPawnOwner());
	RefreshAnimationStateFrame();
}

void UGGYGOAnimInstanceBase::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);

	ACharacter* CurrentOwner = Cast<ACharacter>(TryGetPawnOwner());
	if (CharacterOwner.Get() != CurrentOwner)
	{
		CharacterOwner = CurrentOwner;
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

void UGGYGOAnimInstanceBase::RefreshAnimationStateFrame()
{
	StateCapture.Capture(AnimationState, AnimationDebug, CharacterOwner.Get());
}

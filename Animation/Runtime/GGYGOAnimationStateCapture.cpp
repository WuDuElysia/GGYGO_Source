/**
 * @file GGYGOAnimationStateCapture.cpp
 * @brief 动画语义帧抓取实现
 */
#include "Animation/Runtime/GGYGOAnimationStateCapture.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Animation/Debug/GGYGOAnimationDebugFrame.h"
#include "Animation/Runtime/GGYGOAnimationStateFrame.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "GameFramework/Character.h"

void FGGYGOAnimationStateCapture::Capture(
	FGGYGOAnimationStateFrame& OutState,
	FGGYGOAnimationDebugFrame& OutDebug,
	const ACharacter* InOwner) const
{
	check(IsInGameThread());

	OutState = FGGYGOAnimationStateFrame();
	OutDebug = FGGYGOAnimationDebugFrame();

	if (!InOwner)
	{
		return;
	}

	if (const UAbilitySystemComponent* AbilitySystem =
		UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(InOwner, true))
	{
		AbilitySystem->GetOwnedGameplayTags(OutState.OwnedStateTags);
	}

	const UGGYGOCharacterMovementComponent* MoveComp =
		Cast<UGGYGOCharacterMovementComponent>(InOwner->GetCharacterMovement());
	if (!MoveComp)
	{
		// 编辑器预览可以使用裸 Character。没有项目 CMC 时保持待机默认值。
		return;
	}

	OutState.WorldVelocity = MoveComp->Velocity;
	OutState.WorldAcceleration = MoveComp->GetCurrentAcceleration();
	OutState.HorizontalSpeed = MoveComp->GetHorizontalSpeed();
	OutState.WorldVelocityDirection = MoveComp->GetHorizontalVelocityDirection();
	OutState.LocalVelocityAngle = MoveComp->GetLocalVelocityAngle();
	float LocalBlendX = 0.0f;
	float LocalBlendY = 0.0f;
	MoveComp->GetLocalVelocityBlend(LocalBlendX, LocalBlendY);
	OutState.LocalVelocityBlend = FVector2D(LocalBlendX, LocalBlendY);

	OutState.Gait = MoveComp->GetResolvedGait();
	OutState.MovementMode = MoveComp->MovementMode;
	OutState.TurnBackPhase = MoveComp->GetTurnBackPhase();
	OutState.bHasMoveInput = MoveComp->HasMoveInput();
	OutState.bMovingHorizontally = MoveComp->IsMovingHorizontally();
	OutState.bGrounded = MoveComp->IsMovingOnGround();
	OutState.bMovementBlocked = MoveComp->IsMovementBlockedByTag();
	OutState.bTurnBackRunOut = MoveComp->IsTurnBackRunOut();

	const FVector MoveIntent = OutState.WorldAcceleration.GetSafeNormal2D();
	const FVector ActorForward = InOwner->GetActorForwardVector().GetSafeNormal2D();
	if (!MoveIntent.IsNearlyZero() && !ActorForward.IsNearlyZero())
	{
		OutDebug.InputForwardDot = FMath::Clamp(
			FVector::DotProduct(ActorForward, MoveIntent),
			-1.0f,
			1.0f);
	}

	const FGGYGOAnimCurveMotion& CurveMotion = MoveComp->GetCurveMotion();
	OutDebug.CurveVelocity = CurveMotion.Velocity;
	OutDebug.CurveVelocityDirection = CurveMotion.Direction;
	OutDebug.CurveVelocityAngle = CurveMotion.DirectionAngle;
}

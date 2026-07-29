/**
 * @file ZZZAnimInstance.cpp
 * @brief ZZZ 风格战斗动画 C++ 决策层实现（框架）
 */

#include "Animation/ZZZAnimInstance.h"
#include "BaseCharacter.h"

DEFINE_LOG_CATEGORY_STATIC(LogZZZAnim, Log, All);

void UZZZAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
	Owner = Cast<ABaseCharacter>(TryGetPawnOwner());
}

void UZZZAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);

	// TODO: Capture snapshot from Owner + RuntimeData
	// TODO: Select one-shot assets (combat sequences)
}

void UZZZAnimInstance::NativeThreadSafeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeThreadSafeUpdateAnimation(DeltaSeconds);

	// TODO: Compute numeric outputs from snapshot
}

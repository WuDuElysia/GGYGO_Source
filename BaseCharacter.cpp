/**
 * @file BaseCharacter.cpp
 * @brief 旧角色基类过渡壳实现
 */
#include "BaseCharacter.h"

#include "Character/Components/GGYGOCharacterMovementComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(BaseCharacter)

ABaseCharacter::ABaseCharacter()
{
	// 有意为空。组件与 Tick 设置全部由 AGGYGOCharacterBase 的构造函数完成。
	//
	// 这里**不能**再创建 ASC 或 AttributeSet：父类已经有一个 ASC，
	// 再创建一个会让 IAbilitySystemInterface 的返回值与实际接收 GE 的对象
	// 不是同一个，表现为"GE 应用成功但属性不变"。
}

float ABaseCharacter::GetCurrentSpeed() const
{
	return GetGGYGOMovementComponent()->GetHorizontalSpeed();
}

float ABaseCharacter::GetMoveAngle() const
{
	return GetGGYGOMovementComponent()->GetLocalVelocityAngle();
}

bool ABaseCharacter::IsMoving() const
{
	return GetGGYGOMovementComponent()->IsMovingHorizontally();
}

bool ABaseCharacter::IsGrounded() const
{
	return GetGGYGOMovementComponent()->IsMovingOnGround();
}

EGGYGOGait ABaseCharacter::GetResolvedGait() const
{
	return GetGGYGOMovementComponent()->GetResolvedGait();
}

void ABaseCharacter::NotifyCanYaw()
{
	// 有意为空。转身相位机尚未在 CMC 内实现，本通知目前无人消费。
}
